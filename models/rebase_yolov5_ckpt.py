#!/usr/bin/env python3
"""Re-save a YOLOv5 checkpoint made with the `yolov5` pip package so the Rockchip fork
(github.com/airockchip/yolov5) can export it.

  cd rk_yolov5 && python rebase_yolov5_ckpt.py in.pt out.pt
"""
import os
import sys
import types

import torch

sys.path.insert(0, os.getcwd())  # run from the rk_yolov5 checkout
import models  # noqa: E402
import utils  # noqa: E402
from models.yolo import DetectionModel  # noqa: E402

# alias the pip-package module names onto the fork so the pickle loads
pkg = types.ModuleType("yolov5")
pkg.models, pkg.utils = models, utils
sys.modules["yolov5"] = pkg
for name in list(sys.modules):
    if name.startswith(("models", "utils")):
        sys.modules["yolov5." + name] = sys.modules[name]

src, dst = sys.argv[1], sys.argv[2]
ckpt = torch.load(src, map_location="cpu", weights_only=False)
old = (ckpt.get("ema") or ckpt["model"]).float()
new = DetectionModel(cfg=old.yaml, ch=3, nc=old.yaml["nc"])
new.load_state_dict(old.state_dict(), strict=True)
new.names = getattr(old, "names", None)
new.stride = old.stride
torch.save({"model": new.half(), "epoch": -1}, dst)
print("saved", dst, "classes:", new.names)
