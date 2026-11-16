#!/usr/bin/env python3
"""Makes the tests' tiny models (never committed): every type the protocol carries, a free dimension "batch" to
override, and a few real operators.

    python3 tests/make_test_model.py <folder>

writes <folder>/tiny.onnx and <folder>/other/tiny.onnx (the same graph with other weights: the same name, another
file, for the different-file test), and <folder>/stateful.onnx, a small model with a line's state laid out as BFA
7.0.0's Performance model's (protocol version 3's held mode):

    x:  float [batch, 8]  ->  y:  float [batch, 3]  = tanh(x . W + B)
    id: int64 [1]         ->  z:  int64 [1]         = id + 1
    h:  float16 [batch,4] ->  hh: float16 [batch,4] = h * 2
    d:  double [2]        ->  dd: double [2]        = d + 0.5
    i:  int32 [3]         ->  ii: int32 [3]         = i * 3

stateful.onnx: F = 4 frames a run, a state window of W = 8 frames.

    mel: float [1, 4, 8], identity: int64 [1], emotion: float [1, 4, 2],
    past_k, past_v: float [2, 1, 2, 8, 3], past_frames: int64 [1]
    ->  normalized_motion: float [1, 4, 5], next_k, next_v: float [2, 1, 2, 8, 3]

    next_X = the last 4 frames of past_X, then 4 new frames from mel (the model shifts its own state, as BFA's does)
    normalized_motion = tanh(mel . Wm + emotion . We + identity * Wi + 0.05 * the sum of past_k and past_v over their
                             last past_frames frames), so a start (past_frames 0) ignores the state's contents
"""

import os
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def build(seed):
    rng = np.random.default_rng(seed)
    w = rng.standard_normal((8, 3)).astype(np.float32)
    b = rng.standard_normal((3,)).astype(np.float32)
    inits = [
        numpy_helper.from_array(w, "W"),
        numpy_helper.from_array(b, "B"),
        numpy_helper.from_array(np.array([1], dtype=np.int64), "one"),
        numpy_helper.from_array(np.array([2], dtype=np.float16), "two"),
        numpy_helper.from_array(np.array([0.5], dtype=np.float64), "half"),
        numpy_helper.from_array(np.array([3], dtype=np.int32), "three"),
    ]
    nodes = [
        helper.make_node("MatMul", ["x", "W"], ["xw"]),
        helper.make_node("Add", ["xw", "B"], ["xwb"]),
        helper.make_node("Tanh", ["xwb"], ["y"]),
        helper.make_node("Add", ["id", "one"], ["z"]),
        helper.make_node("Mul", ["h", "two"], ["hh"]),
        helper.make_node("Add", ["d", "half"], ["dd"]),
        helper.make_node("Mul", ["i", "three"], ["ii"]),
    ]
    inputs = [
        helper.make_tensor_value_info("x", TensorProto.FLOAT, ["batch", 8]),
        helper.make_tensor_value_info("id", TensorProto.INT64, [1]),
        helper.make_tensor_value_info("h", TensorProto.FLOAT16, ["batch", 4]),
        helper.make_tensor_value_info("d", TensorProto.DOUBLE, [2]),
        helper.make_tensor_value_info("i", TensorProto.INT32, [3]),
    ]
    outputs = [
        helper.make_tensor_value_info("y", TensorProto.FLOAT, ["batch", 3]),
        helper.make_tensor_value_info("z", TensorProto.INT64, [1]),
        helper.make_tensor_value_info("hh", TensorProto.FLOAT16, ["batch", 4]),
        helper.make_tensor_value_info("dd", TensorProto.DOUBLE, [2]),
        helper.make_tensor_value_info("ii", TensorProto.INT32, [3]),
    ]
    graph = helper.make_graph(nodes, "tiny", inputs, outputs, inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="bfa-remote-bridge tests")
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def build_stateful(seed):
    rng = np.random.default_rng(seed)
    f32 = np.float32
    inits = [
        numpy_helper.from_array(rng.standard_normal((8, 12)).astype(f32), "Wk"),
        numpy_helper.from_array(rng.standard_normal((8, 12)).astype(f32), "Wv"),
        numpy_helper.from_array(rng.standard_normal((8, 5)).astype(f32), "Wm"),
        numpy_helper.from_array(rng.standard_normal((2, 5)).astype(f32), "We"),
        numpy_helper.from_array(rng.standard_normal((5,)).astype(f32), "Wi"),
        numpy_helper.from_array(np.array([1, 4, 2, 2, 3], dtype=np.int64), "split_shape"),
        numpy_helper.from_array(np.array([4], dtype=np.int64), "keep_from"),
        numpy_helper.from_array(np.array([8], dtype=np.int64), "keep_to"),
        numpy_helper.from_array(np.array([3], dtype=np.int64), "frame_axis"),
        numpy_helper.from_array(np.arange(8, dtype=np.int64), "slots"),
        numpy_helper.from_array(np.array([8], dtype=np.int64), "window"),
        numpy_helper.from_array(np.array([1, 1, 1, 8, 1], dtype=np.int64), "mask_shape"),
        numpy_helper.from_array(np.array(0.05, dtype=f32), "weight"),
    ]
    nodes = []
    for x in ("k", "v"):
        nodes += [
            helper.make_node("MatMul", ["mel", "W" + x], [x + "_mm"]),
            helper.make_node("Tanh", [x + "_mm"], [x + "_t"]),
            helper.make_node("Reshape", [x + "_t", "split_shape"], [x + "_5"]),
            helper.make_node("Transpose", [x + "_5"], [x + "_new"], perm=[2, 0, 3, 1, 4]),
            helper.make_node("Slice", ["past_" + x, "keep_from", "keep_to", "frame_axis"], [x + "_old"]),
            helper.make_node("Concat", [x + "_old", x + "_new"], ["next_" + x], axis=3),
        ]
    nodes += [
        # the valid frames: the last past_frames of the window
        helper.make_node("Sub", ["window", "past_frames"], ["first_valid"]),
        helper.make_node("GreaterOrEqual", ["slots", "first_valid"], ["valid"]),
        helper.make_node("Cast", ["valid"], ["valid_f"], to=TensorProto.FLOAT),
        helper.make_node("Reshape", ["valid_f", "mask_shape"], ["mask"]),
        helper.make_node("Mul", ["past_k", "mask"], ["k_masked"]),
        helper.make_node("Mul", ["past_v", "mask"], ["v_masked"]),
        helper.make_node("ReduceSum", ["k_masked"], ["k_sum"], keepdims=0),
        helper.make_node("ReduceSum", ["v_masked"], ["v_sum"], keepdims=0),
        helper.make_node("Add", ["k_sum", "v_sum"], ["state_sum"]),
        helper.make_node("Mul", ["state_sum", "weight"], ["state_term"]),
        helper.make_node("MatMul", ["mel", "Wm"], ["mel_term"]),
        helper.make_node("MatMul", ["emotion", "We"], ["emotion_term"]),
        helper.make_node("Cast", ["identity"], ["identity_f"], to=TensorProto.FLOAT),
        helper.make_node("Mul", ["identity_f", "Wi"], ["identity_term"]),
        helper.make_node("Add", ["mel_term", "emotion_term"], ["m1"]),
        helper.make_node("Add", ["m1", "identity_term"], ["m2"]),
        helper.make_node("Add", ["m2", "state_term"], ["m3"]),
        helper.make_node("Tanh", ["m3"], ["normalized_motion"]),
    ]
    state = [2, 1, 2, 8, 3]
    inputs = [
        helper.make_tensor_value_info("mel", TensorProto.FLOAT, [1, 4, 8]),
        helper.make_tensor_value_info("identity", TensorProto.INT64, [1]),
        helper.make_tensor_value_info("emotion", TensorProto.FLOAT, [1, 4, 2]),
        helper.make_tensor_value_info("past_k", TensorProto.FLOAT, state),
        helper.make_tensor_value_info("past_v", TensorProto.FLOAT, state),
        helper.make_tensor_value_info("past_frames", TensorProto.INT64, [1]),
    ]
    outputs = [
        helper.make_tensor_value_info("normalized_motion", TensorProto.FLOAT, [1, 4, 5]),
        helper.make_tensor_value_info("next_k", TensorProto.FLOAT, state),
        helper.make_tensor_value_info("next_v", TensorProto.FLOAT, state),
    ]
    graph = helper.make_graph(nodes, "stateful", inputs, outputs, inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="bfa-remote-bridge tests")
    model.ir_version = 8
    onnx.checker.check_model(model, full_check=True)
    return model


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "other"), exist_ok=True)
    onnx.save(build(1), os.path.join(out, "tiny.onnx"))
    onnx.save(build(2), os.path.join(out, "other", "tiny.onnx"))
    onnx.save(build_stateful(3), os.path.join(out, "stateful.onnx"))


if __name__ == "__main__":
    main()
