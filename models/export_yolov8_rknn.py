#!/usr/bin/env python3
"""Export an Ultralytics YOLOv8/YOLO11 detection model to the ONNX layout Rockchip NPUs like
(same as github.com/airockchip/ultralytics_yolo11): per scale a raw DFL box map (64 ch), a sigmoid
class map and a clamped class-sum map. No decoding inside the graph, so INT8 stays accurate.

  export_yolov8_rknn.py model.pt out.onnx [imgsz=320]
"""
import sys
import types

import torch
from ultralytics import YOLO

pt, out = sys.argv[1], sys.argv[2]
imgsz = int(sys.argv[3]) if len(sys.argv) > 3 else 320

model = YOLO(pt).model.float().eval()
for p in model.parameters():
    p.requires_grad_(False)
det = model.model[-1]


def rknn_forward(self, x):
    outs = []
    for i in range(self.nl):
        box = self.cv2[i](x[i])
        cls = self.cv3[i](x[i]).sigmoid()
        outs += [box, cls, torch.clamp(cls.sum(1, keepdim=True), 0, 1)]
    return outs


det.forward = types.MethodType(rknn_forward, det)
names = [f"{k}{i}" for i in range(det.nl) for k in ("box", "cls", "sum")]
torch.onnx.export(model, torch.zeros(1, 3, imgsz, imgsz), out, opset_version=12, input_names=["images"],
                  output_names=names)
print("saved", out, "classes:", model.names)
