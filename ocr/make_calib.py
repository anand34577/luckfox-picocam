#!/usr/bin/env python3
"""Write INT8 calibration images for the OCR model: real crops + synthetic plates, all 128x32.

  python make_calib.py real/test.txt calib_ocr/ 200  -> calib_ocr/*.png + calib_ocr.txt
"""
import os
import sys

import cv2

from plategen import H, W, make_sample
from train import load_labelled

labels, out, n_synth = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(out, exist_ok=True)
paths = []
for i, (img, _) in enumerate(load_labelled(labels)):
    p = os.path.abspath(os.path.join(out, f"real_{i:03d}.png"))
    cv2.imwrite(p, img[:, :, ::-1])
    paths.append(p)
for i in range(n_synth):
    img, _ = make_sample("fonts")
    p = os.path.abspath(os.path.join(out, f"synth_{i:03d}.png"))
    cv2.imwrite(p, cv2.resize(img, (W, H))[:, :, ::-1])
    paths.append(p)
open(out.rstrip("/") + ".txt", "w").write("\n".join(paths) + "\n")
print(len(paths), "calibration images")
