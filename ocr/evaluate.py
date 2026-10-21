#!/usr/bin/env python3
"""Score a PlateNet checkpoint on labelled real crops; optionally export ONNX for RKNN.

  python evaluate.py platenet.pt real/test.txt [--onnx platenet.onnx]
"""
import argparse

import numpy as np
import torch

from train import PlateNet, evaluate, load_labelled

ap = argparse.ArgumentParser()
ap.add_argument("ckpt")
ap.add_argument("labels")
ap.add_argument("--onnx", default="")
a = ap.parse_args()

model = PlateNet()
model.load_state_dict(torch.load(a.ckpt, map_location="cpu"))
items = load_labelled(a.labels)
acc, pairs = evaluate(model, items, "cpu")
cer = np.mean([sum(x != y for x, y in zip(p.ljust(len(t)), t)) / len(t) for p, t in pairs]) if pairs else 0
print(f"exact match {acc:.2%}  |  char error rate {cer:.2%}  ({len(items)} crops)")
for p, t in pairs:
    if p != t:
        print(f"  {t:12s} -> {p}")
if a.onnx:
    model.eval()
    torch.onnx.export(model, torch.zeros(1, 3, 32, 128), a.onnx, opset_version=12, input_names=["image"],
                      output_names=["logits"])
    print("saved", a.onnx, "(output [1, 37, 32], blank = last class)")
