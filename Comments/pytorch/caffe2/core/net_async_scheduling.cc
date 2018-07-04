#include "caffe2/core/net_async_scheduling.h"

#include "caffe2/core/net_async_tracing.h"

CAFFE2_DEFINE_bool(
    caffe2_net_async_optimize_polling,
    true,
    "Use event callbacks whenever possible instead of polling");

namespace caffe2 {

AsyncSchedulingNet::AsyncSchedulingNet(
    const std::shared_ptr<const NetDef>& net_def,
    Workspace* ws)
    : AsyncNetBase(net_def, ws), running_(false), use_dfs_scheduling_(false) {
  for (int arg_idx = 0; arg_idx < net_def->arg_size(); ++arg_idx) {
    auto& arg = net_def->arg(arg_idx);
    if (arg.has_name() && arg.name() == "deferrable_mode") {
      CAFFE_ENFORCE(arg.has_i(), "deferrable_mode should be an int");
      use_dfs_scheduling_ = arg.i() == 1; // corr. to DFS scheduling
      break;
    }
  }
}

void AsyncSchedulingNet::reset() {
  AsyncNetBase::reset();
  processed_tasks_num_ = 0;
}

void AsyncSchedulingNet::Wait() {
  std::unique_lock<std::mutex> lock(running_mutex_);
  while (running_) {
    running_cv_.wait(lock);
  }
}

void AsyncSchedulingNet::schedule(int task_id, bool run_inline) {
  if (!testAndSetScheduled(task_id)) {
    return;
  }
  auto schedule_func = [this, task_id]() {
    if (success_) {
      int stream_id = 0;
      if (streams_per_gpu_ > 1) {
        stream_id = stream(task_id);
      }
      if (!run(task_id, stream_id)) {
        success_ = false;
      }
    }

    for (auto child_id : children(task_id)) {
      int parent_count = updateParentCount(child_id);
      if (parent_count == 0) {
        // Schedule a child if:
        // - there is failure, we skip an op execution and finish the job
        // - forced scheduling though --caffe2_net_async_always_schedule_child
        // - --caffe2_net_async_finish_chain is set, in this case parents are
        //   guaranteed to be finished
        // - in all other cases, check parents with canSchedule
        if (!success_ || always_schedule_child_ || finish_chain_ ||
            canSchedule(child_id)) {
          // if DFS scheduling is enabled, run children inline,
          // ignore DFS scheduling in callbacks
          schedule(child_id, use_dfs_scheduling_);
        } else {
          bool parent_failed = false;
          bool parent_needs_polling = false;
          std::vector<int> parents_with_callback;

          for (auto parent_id : parents(child_id)) {
            auto& parent_event = event(parent_id);
            auto parent_status = parent_event.Query();

            if (parent_status == EventStatus::EVENT_FAILED) {
              parent_failed = true;
              break;
            } else if (parent_status == EventStatus::EVENT_SCHEDULED) {
              // parent is not finished yet, check if this is blocking us
              // from scheduling a child
              if (!canSchedule(parent_id, child_id)) {
                // we can't schedule a child because of this parent,
                // check if parent supports callback
                if (FLAGS_caffe2_net_async_optimize_polling &&
                    parent_event.SupportsCallback()) {
                  parents_with_callback.push_back(parent_id);
                } else {
                  parent_needs_polling = true;
                  break;
                }
              }
            } else if (parent_status != EventStatus::EVENT_SUCCESS) {
              VLOG(1) << "Unexpected parent task state: " << parent_status
                      << ", task id: " << child_id
                      << ", parent task id: " << parent_id;
              parent_failed = true;
              break;
            }
          }

          if (parent_failed) {
            // one of parents failed, set failure flag and wrap up execution
            success_ = false;
            schedule(child_id, use_dfs_scheduling_);
          } else if (parent_needs_polling) {
            // some parents are blocking us from scheduling a child and don't
            // support callbacks, using polling
            const auto& child_device_option = event(child_id).GetDeviceOption();
            pool(child_device_option)
                ->run(std::bind(
                    &AsyncSchedulingNet::pollAndSchedule, this, child_id));
          } else if (!parents_with_callback.empty()) {
            // some parents are blocking us from scheduling a child and they
            // support callbacks
            for (auto parent_id : parents_with_callback) {
              event(parent_id).SetCallback(std::bind(
                  &AsyncSchedulingNet::parentCallback, this, parent_id));
            }
          } else {
            // we're ready to schedule a child
            schedule(child_id, use_dfs_scheduling_);
          }
        }
      }
    }

    // finishRun may cause waiters to wake up and destroy the net,
    // before we call finishRun we need to make sure all other (finishing)
    // tasks are done;
    // Bumping and checking the counter after the task's job is done
    auto tasks_num = tasksNum();
    auto cur_processed_tasks = ++processed_tasks_num_;
    if (cur_processed_tasks == tasks_num) {
      finishRun();
    }
  };

  if (run_inline) {
    schedule_func();
  } else {
    const auto& device_option = event(task_id).GetDeviceOption();
    pool(device_option)->run(schedule_func);
  }
}

void AsyncSchedulingNet::parentCallback(int parent_id) {
  if (event(parent_id).Query() != EventStatus::EVENT_SUCCESS) {
    success_ = false;
  }
  for (auto child_id : children(parent_id)) {
    int parent_count = getParentCount(child_id);
    if (parent_count == 0) {
      if (!success_ || canSchedule(child_id)) {
        schedule(child_id);
      }
    }
  }
}

void AsyncSchedulingNet::pollAndSchedule(int task_id) {
  bool parent_failed = false;
  bool can_schedule = canSchedule(task_id, nullptr, &parent_failed);
  if (parent_failed) {
    success_ = false;
  }
  // schedule the task if:
  //  - parents are ready
  //  - we failed / cleanup started (no ops will run)

  if (can_schedule || !success_ || parent_failed) {
    schedule(task_id);
  } else {
    const auto& device_option = event(task_id).GetDeviceOption();
    pool(device_option)
        ->run(std::bind(&AsyncSchedulingNet::pollAndSchedule, this, task_id));
  }
}

void AsyncSchedulingNet::finishRun() {
  {
    std::unique_lock<std::mutex> lock(running_mutex_);
    running_ = false;
  }
  // wait for scheduled ops and make sure all events are marked as finished
  finalizeEvents();
  // notify observers and waiters
  StopAllObservers();
  running_cv_.notify_all();
}

bool AsyncSchedulingNet::RunAsync() {
  try {
    std::unique_lock<std::mutex> lock(running_mutex_);
    if (running_) {
      LOG(ERROR) << "Detected concurrent runs";
      return false;
    }
    running_ = true;
    reset();

    StartAllObservers();
    tracing::startIter(tracer_);

    for (auto task_id = 0; task_id < tasksNum(); ++task_id) {
      if (parents(task_id).empty()) {
        schedule(task_id);
      }
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Exception while starting an async run: " << e.what();
    finishRun();
    return false;
  }

  if (tasksNum() == 0) {
    finishRun();
  }

  if (is_blocking_) {
    Wait();
  }

  return true;
}

AsyncSchedulingNet::~AsyncSchedulingNet() {
  Wait();
}

REGISTER_NET(async_scheduling, AsyncSchedulingNet);

} // namespace caffe2
