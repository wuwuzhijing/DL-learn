#include "caffe2/opt/optimize_ideep.h"
#include "caffe2/opt/converter.h"
#include "caffe2/opt/fusion.h"
#include "caffe2/utils/proto_utils.h"

namespace caffe2 {
namespace opt {

using namespace nom;

void OptimizeForIdeep(repr::NNModule* nn) {
  // Conv+Relu fusion
  auto should_fuse = [](const repr::Conv& conv) {
    const auto annotation = conv.getAnnotation();
    if (!annotation || !isa<Caffe2Annotation>(annotation)) {
      return false;
    }
    const auto& op = dyn_cast<Caffe2Annotation>(annotation)->getOperatorDef();

    // We only want to fuse for IDEEP convs
    if (op.device_option().device_type() != DeviceType::IDEEP) {
      return false;
    }

    // IDEEP doesn't support fusion group conv
    int group =
        ArgumentHelper::GetSingleArgument<OperatorDef, int>(op, "group", 1);
    if (group != 1) {
      return false;
    }

    return true;
  };
  auto postprocess = [](repr::NNGraph::NodeRef conv_node) {
    auto conv = repr::nn::get<repr::Conv>(conv_node);
    auto annotation = conv->getMutableAnnotation();
    if (!annotation || !isa<Caffe2Annotation>(annotation)) {
      return;
    }
    auto* op = dyn_cast<Caffe2Annotation>(annotation)->getMutableOperatorDef();
    op->set_type("ConvFusion");
    auto* arg = op->add_arg();
    arg->set_name("fusion_type");
    // 1 means FUSION_CONV_RELU
    arg->set_i(1);
  };
  fuseActivation<repr::Conv, repr::Relu>(nn, should_fuse, postprocess);
}

} // namespace opt
} // namespace caffe2
