from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import unittest
import hypothesis.strategies as st
from hypothesis import given, settings
import numpy as np
from caffe2.python import core, workspace
import caffe2.python.hypothesis_test_util as hu
import caffe2.python.ideep_test_util as mu


@unittest.skipIf(not workspace.C.use_ideep, "No IDEEP support.")
class ConvTest(hu.HypothesisTestCase):
    @given(stride=st.integers(1, 3),
           pad=st.integers(0, 3),
           kernel=st.integers(3, 5),
           size=st.integers(8, 10),
           input_channels=st.integers(1, 3),
           output_channels=st.integers(1, 5),
           batch_size=st.integers(1, 3),
           use_bias=st.booleans(),
           training_mode=st.booleans(),
           group=st.integers(1, 2),
           **mu.gcs)
    def test_convolution(self, stride, pad, kernel, size,
                             input_channels, output_channels,
                             batch_size, use_bias, training_mode, group, gc, dc):
        training = 1 if training_mode else 0
        op = core.CreateOperator(
            "Conv",
            ["X", "w", "b"] if use_bias else ["X", "w"],
            ["Y"],
            stride=stride,
            pad=pad,
            kernel=kernel,
            group=group,
            training_mode=training,
        )
        X = np.random.rand(
            batch_size, input_channels * group, size, size).astype(np.float32) - 0.5
        w = np.random.rand(
                output_channels * group, input_channels, kernel, kernel) \
            .astype(np.float32) - 0.5
        b = np.random.rand(output_channels * group).astype(np.float32) - 0.5

        inputs = [X, w, b] if use_bias else [X, w]
        self.assertDeviceChecks(dc, op, inputs, [0])

        if training_mode:
            for i in range(len(inputs)):
                self.assertGradientChecks(gc, op, inputs, i, [0], threshold=0.01)

    @given(batch_size=st.integers(1, 3), **mu.gcs)
    def test_depthwise_convolution(self, batch_size, gc, dc):
        op = core.CreateOperator(
            "Conv",
            ["X", "w", "b"],
            ["Y"],
            stride=1,
            pad=0,
            kernel=1,
            group=4,
        )
        X = np.random.rand(batch_size, 544, 14, 14).astype(np.float32)
        w = np.random.rand(544, 136, 1, 1).astype(np.float32)
        b = np.random.rand(544).astype(np.float32)
        inputs = [X, w, b]
        self.assertDeviceChecks(dc, op, inputs, [0])

if __name__ == "__main__":
    unittest.main()
