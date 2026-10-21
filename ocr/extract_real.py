#!/usr/bin/env python3
"""Cut plate crops out of recorded 960x540 frames with the YOLO11 plate detector, the way edgeai
crops them (box expanded 6% x / 12% y). Keeps the largest view of each plate pass.

  python extract_real.py lp11n.pt realframes real/  -> real/*.jpg + real/contact.jpg (to label)
"""
import glob
import os
import sys

import cv2
import numpy as np
from ultralytics import YOLO

weights, frames_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
model = YOLO(weights)


def iou(a, b):
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    i = ix * iy
    return i / ((a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - i + 1e-6)


keep = []  # (width, crop, stream, frame)
for stream in sorted({os.path.basename(p).split("_")[0] for p in glob.glob(f"{frames_dir}/*.jpg")}):
    files = sorted(glob.glob(f"{frames_dir}/{stream}_*.jpg"))
    active = []  # [box, best_w, best_crop, last_seen]
    for fi, f in enumerate(files):
        img = cv2.imread(f)
        for b in model.predict(img, imgsz=320, conf=0.35, verbose=False)[0].boxes:
            x1, y1, x2, y2 = b.xyxy[0].tolist()
            w, h = x2 - x1, y2 - y1
            if w < 30:
                continue
            ex = [max(0, x1 - 0.06 * w), max(0, y1 - 0.12 * h), min(960, x2 + 0.06 * w), min(540, y2 + 0.12 * h)]
            crop = img[int(ex[1]):int(ex[3]), int(ex[0]):int(ex[2])]
            for t in active:
                if iou(t[0], (x1, y1, x2, y2)) > 0.2 and fi - t[3] <= 3:
                    t[0], t[3] = (x1, y1, x2, y2), fi
                    if w > t[1]:
                        t[1], t[2] = w, crop
                    break
            else:
                active.append([(x1, y1, x2, y2), w, crop, fi])
        for t in [t for t in active if fi - t[3] > 3]:
            keep.append((t[1], t[2], stream, t[3]))
            active.remove(t)
    keep += [(t[1], t[2], stream, t[3]) for t in active]

keep.sort(key=lambda k: -k[0])
tiles = []
with open(os.path.join(out, "labels_todo.txt"), "w") as fp:
    for i, (w, crop, stream, fi) in enumerate(keep):
        name = f"{i:03d}_{stream}_{fi:04d}.jpg"
        cv2.imwrite(os.path.join(out, name), crop)
        fp.write(f"{name} ?\n")
        t = cv2.resize(crop, (240, 60))
        cv2.putText(t, str(i), (2, 14), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 0, 255), 1)
        tiles.append(t)
while len(tiles) % 4:
    tiles.append(np.zeros((60, 240, 3), np.uint8))
if tiles:
    cv2.imwrite(os.path.join(out, "contact.jpg"), np.vstack([np.hstack(tiles[i:i + 4]) for i in range(0, len(tiles), 4)]))
print(len(keep), "plate crops")
