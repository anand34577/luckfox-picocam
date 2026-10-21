#!/usr/bin/env python3
"""Train the edgeai plate OCR: a small all-convolutional CTC recogniser (RV1103 NPU friendly).

  python train.py --fonts fonts --steps 30000 [--real real/labels.txt] --out platenet.pt
Input 3x32x128 RGB, output [N, 37, 32] (36 chars + CTC blank as the last class).
"""
import argparse
import os
import random
import time

import cv2
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, IterableDataset

from plategen import CHARSET, H, W, make_sample

cv2.setNumThreads(0)  # data workers run in parallel processes; avoid thread oversubscription

BLANK = len(CHARSET)  # last class, as the device decoder expects


def cbr(i, o, k=3, p=1):
    return nn.Sequential(nn.Conv2d(i, o, k, 1, p, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))


class PlateNet(nn.Module):
    def __init__(self, ncls=len(CHARSET) + 1):
        super().__init__()
        self.f = nn.Sequential(
            cbr(3, 32), nn.MaxPool2d(2),                        # 16x64
            cbr(32, 64), nn.MaxPool2d(2),                       # 8x32
            cbr(64, 96), cbr(96, 96), nn.MaxPool2d((2, 1)),     # 4x32
            cbr(96, 128), cbr(128, 128), nn.MaxPool2d((2, 1)),  # 2x32
            cbr(128, 160, k=(2, 3), p=(0, 1)),                  # 1x32: collapse height
            cbr(160, 160, k=(1, 3), p=(0, 1)),                  # horizontal context
            cbr(160, 160, k=(1, 3), p=(0, 1)),
            nn.Dropout(0.1),
            nn.Conv2d(160, ncls, 1),
        )

    def forward(self, x):  # x: N,3,32,128 normalised
        return self.f(x).squeeze(2)  # N, C, T


def normalise(img_rgb):
    return (torch.from_numpy(img_rgb).permute(2, 0, 1).float() - 127.5) / 127.5


class Synth(IterableDataset):
    def __init__(self, fonts, real=None, real_ratio=0.0):
        self.fonts, self.real, self.real_ratio = fonts, real or [], real_ratio

    def __iter__(self):
        torch.set_num_threads(1)
        seed = (torch.utils.data.get_worker_info().id if torch.utils.data.get_worker_info() else 0) + int(time.time())
        random.seed(seed)
        np.random.seed(seed % (2 ** 31))
        while True:
            if self.real and random.random() < self.real_ratio:
                img, txt = random.choice(self.real)
                img = augment_real(img)
            else:
                img, txt = make_sample(self.fonts)
            yield normalise(img), txt


def augment_real(img):
    img = img.astype(np.float32) * random.uniform(0.7, 1.3) + random.uniform(-30, 30)
    img = np.clip(img, 0, 255).astype(np.uint8)
    h, w = img.shape[:2]
    dx, dy = random.randint(-3, 3), random.randint(-2, 2)
    M = np.float32([[1, 0, dx], [0, 1, dy]])
    return cv2.warpAffine(img, M, (w, h), borderMode=cv2.BORDER_REPLICATE)


def collate(batch):
    imgs = torch.stack([b[0] for b in batch])
    targets = [torch.tensor([CHARSET.index(c) for c in b[1]]) for b in batch]
    lens = torch.tensor([len(t) for t in targets])
    return imgs, torch.cat(targets), lens, [b[1] for b in batch]


def greedy(logits):  # N,C,T -> strings
    out = []
    for seq in logits.argmax(1).cpu().numpy():
        s, prev = "", -1
        for k in seq:
            if k != BLANK and k != prev:
                s += CHARSET[k]
            prev = k
        out.append(s)
    return out


def load_labelled(path):
    """labels.txt lines: <image path> <TEXT>  (relative to the file)."""
    items = []
    if not path or not os.path.exists(path):
        return items
    base = os.path.dirname(path)
    for line in open(path):
        parts = line.strip().split()
        if len(parts) != 2:
            continue
        img = cv2.imread(os.path.join(base, parts[0]))
        if img is None:
            continue
        items.append((cv2.resize(img[:, :, ::-1], (W, H)), parts[1].upper()))
    return items


def evaluate(model, items, dev):
    if not items:
        return None, []
    model.eval()
    with torch.no_grad():
        x = torch.stack([normalise(i) for i, _ in items]).to(dev)
        pred = greedy(model(x))
    model.train()
    ok = sum(p == t for p, (_, t) in zip(pred, items))
    return ok / len(items), list(zip(pred, [t for _, t in items]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", default="fonts")
    ap.add_argument("--steps", type=int, default=30000)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--real", default="", help="labels.txt of real crops used for training")
    ap.add_argument("--real-ratio", type=float, default=0.15)
    ap.add_argument("--test", default="", help="labels.txt of real crops used only for evaluation")
    ap.add_argument("--init", default="")
    ap.add_argument("--out", default="platenet.pt")
    ap.add_argument("--workers", type=int, default=7)
    a = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    model = PlateNet().to(dev)
    if a.init:
        model.load_state_dict(torch.load(a.init, map_location=dev))
    print("params: %.2fM" % (sum(p.numel() for p in model.parameters()) / 1e6), "device:", dev)
    real = load_labelled(a.real)
    test = load_labelled(a.test)
    print(f"real train crops: {len(real)}, real test crops: {len(test)}")

    dl = DataLoader(Synth(a.fonts, real, a.real_ratio if real else 0), batch_size=a.batch, num_workers=a.workers,
                    collate_fn=collate, persistent_workers=True, prefetch_factor=4)
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps, pct_start=0.08)
    ctc = nn.CTCLoss(blank=BLANK, zero_infinity=True)
    t0, best = time.time(), -1.0
    for step, (x, tg, lens, txt) in enumerate(dl, 1):
        x = x.to(dev)
        logits = model(x)  # N,C,T
        logp = logits.permute(2, 0, 1).log_softmax(2)  # T,N,C
        T = logp.shape[0]
        loss = ctc(logp, tg, torch.full((x.shape[0],), T, dtype=torch.long), lens)
        opt.zero_grad()
        loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), 5.0)
        opt.step()
        sched.step()
        if step % 500 == 0 or step == a.steps:
            acc_s = np.mean([p == t for p, t in zip(greedy(logits), txt)])
            acc_r, _ = evaluate(model, test, dev)
            msg = f"step {step}/{a.steps} loss {loss.item():.3f} synth-acc {acc_s:.2f}"
            if acc_r is not None:
                msg += f" real-acc {acc_r:.2f}"
            print(msg, f"({time.time() - t0:.0f}s)", flush=True)
            score = acc_r if acc_r is not None else acc_s
            if score >= best:
                best = score
                torch.save(model.state_dict(), a.out)
        if step >= a.steps:
            break
    model.load_state_dict(torch.load(a.out, map_location=dev))
    acc_r, pairs = evaluate(model, test, dev)
    if pairs:
        print(f"best real-crop accuracy: {acc_r:.2f}")
        for p, t in pairs:
            print(("  ok  " if p == t else "  BAD ") + f"{t:12s} -> {p}")
    print("saved", a.out)


if __name__ == "__main__":
    main()
