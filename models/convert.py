#!/usr/bin/env python3
"""Convert an ONNX model to RKNN (INT8) for Luckfox Pico (RV1103/RV1106).

  convert.py model.onnx calib.txt out.rknn [--size 320 | --size 94x24] [--preset yolo|retinaface|facenet|lprnet]

calib.txt lists one image path per line (frames from your own camera give the best INT8 accuracy).
"""
import argparse

from rknn.api import RKNN

PRESETS = {
    # mean/std applied on-NPU to uint8 input; rgb2bgr: calibration images are swapped to BGR
    "yolo": dict(mean=[0, 0, 0], std=[255, 255, 255], rgb2bgr=False),
    "retinaface": dict(mean=[104, 117, 123], std=[1, 1, 1], rgb2bgr=True),
    "facenet": dict(mean=[0, 0, 0], std=[255, 255, 255], rgb2bgr=True),
    "lprnet": dict(mean=[127.5, 127.5, 127.5], std=[128, 128, 128], rgb2bgr=True),
    "platenet": dict(mean=[127.5, 127.5, 127.5], std=[127.5, 127.5, 127.5], rgb2bgr=False),  # ocr/train.py
}

ap = argparse.ArgumentParser()
ap.add_argument("onnx")
ap.add_argument("calib")
ap.add_argument("out")
ap.add_argument("--size", default=None, help="input size: 320 or WxH (default: keep the ONNX shape)")
ap.add_argument("--preset", default="yolo", choices=PRESETS)
ap.add_argument("--platform", default="rv1103", choices=["rv1103", "rv1106"],
                help="target chip: rv1103 or rv1106 (the web UI shows which one your board is). A model only runs on the chip it was built for")
ap.add_argument("--disable-rule", action="append", default=[],
                help="graph optimisation rule to skip (YOLO11 attention needs "
                     "fuse_exmatmul_add_mul_exsoftmax13_exmatmul_to_sdpa)")
a = ap.parse_args()
p = PRESETS[a.preset]

rknn = RKNN(verbose=False)
extra = {"disable_rules": a.disable_rule} if a.disable_rule else {}
rknn.config(mean_values=[p["mean"]], std_values=[p["std"]], target_platform=a.platform,
            quant_img_RGB2BGR=p["rgb2bgr"], **extra)
kw = {}
if a.size:
    w, _, h = a.size.partition("x")
    kw["input_size_list"] = [[1, 3, int(h or w), int(w)]]
assert rknn.load_onnx(model=a.onnx, **kw) == 0, "load failed"
assert rknn.build(do_quantization=True, dataset=a.calib) == 0, "build failed"
assert rknn.export_rknn(a.out) == 0, "export failed"
rknn.release()
print("saved", a.out)
