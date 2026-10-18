#!/usr/bin/env python3
"""Makes the tests' tiny models (never committed): every type the protocol carries, a free dimension "batch" to
override, and a few real operators.

    python3 tests/make_test_model.py <folder>

writes <folder>/tiny.onnx and <folder>/other/tiny.onnx (the same graph with other weights: the same name, another
file, for the different-file test).

    x:  float [batch, 8]  ->  y:  float [batch, 3]  = tanh(x . W + B)
    id: int64 [1]         ->  z:  int64 [1]         = id + 1
    h:  float16 [batch,4] ->  hh: float16 [batch,4] = h * 2
    d:  double [2]        ->  dd: double [2]        = d + 0.5
    i:  int32 [3]         ->  ii: int32 [3]         = i * 3
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


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "other"), exist_ok=True)
    onnx.save(build(1), os.path.join(out, "tiny.onnx"))
    onnx.save(build(2), os.path.join(out, "other", "tiny.onnx"))


if __name__ == "__main__":
    main()
