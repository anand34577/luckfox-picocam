#!/usr/bin/env python3
"""Quantise the OCR ONNX like convert.py does and compare simulator (INT8) vs PyTorch on real crops.

  python check_rknn.py platenet.pt platenet.onnx calib_ocr.txt real/test.txt
"""
import sys

import numpy as np
import torch
from rknn.api import RKNN

from train import PlateNet, greedy, load_labelled, normalise

ckpt, onnx, calib, labels = sys.argv[1:5]
items = load_labelled(labels)
model = PlateNet()
model.load_state_dict(torch.load(ckpt, map_location="cpu"))
model.eval()
with torch.no_grad():
    ref = greedy(model(torch.stack([normalise(i) for i, _ in items])))

r = RKNN(verbose=False)
r.config(mean_values=[[127.5] * 3], std_values=[[127.5] * 3], target_platform="rv1103")
r.load_onnx(model=onnx)
r.build(do_quantization=True, dataset=calib)
r.init_runtime()
sim = []
for img, _ in items:
    out = r.inference(inputs=[img[None]], data_format="nhwc")[0]  # (1, 37, 32)
    sim += greedy(torch.from_numpy(out))
agree = np.mean([a == b for a, b in zip(ref, sim)])
acc_t = np.mean([a == t for a, (_, t) in zip(ref, items)])
acc_s = np.mean([a == t for a, (_, t) in zip(sim, items)])
print(f"PyTorch acc {acc_t:.2%} | INT8 simulator acc {acc_s:.2%} | same output {agree:.2%}")
