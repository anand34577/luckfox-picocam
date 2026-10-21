#!/usr/bin/env python3
"""Change a fully-convolutional ONNX model's fixed input size (e.g. RetinaFace 640 -> 320).

  resize_onnx_input.py in.onnx out.onnx 320 [320]
"""
import sys

import onnx
from onnxsim import simplify

src, dst, w = sys.argv[1], sys.argv[2], int(sys.argv[3])
h = int(sys.argv[4]) if len(sys.argv) > 4 else w
m = onnx.load(src)
inp = m.graph.input[0]
name = inp.name
# drop stale shape info so it is re-inferred for the new size
del m.graph.value_info[:]
for o in m.graph.output:
    for i, d in enumerate(o.type.tensor_type.shape.dim):
        d.dim_param = f"{o.name}_d{i}"  # mark unknown; re-inferred below
m_simp, ok = simplify(m, overwrite_input_shapes={name: [1, 3, h, w]})
assert ok, "onnxsim check failed"
onnx.save(m_simp, dst)
print("saved", dst, [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in m_simp.graph.output])
