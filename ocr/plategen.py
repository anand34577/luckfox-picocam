"""Synthetic Indian number-plate generator for OCR training.

Produces (image, text) pairs that look like the crops edgeai sends to the OCR model:
plate box expanded by 6% / 12%, stretched to 128x32, single text row. Two-row plates
(bikes, autos) are split into rows the same way the device does.
"""
import glob
import os
import random

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
W, H = 128, 32

STATES = ["AN", "AP", "AR", "AS", "BR", "CH", "CG", "DD", "DL", "DN", "GA", "GJ", "HR", "HP", "JH", "JK",
          "KA", "KL", "LA", "LD", "MH", "ML", "MN", "MP", "MZ", "NL", "OD", "OR", "PB", "PY", "RJ", "SK",
          "TN", "TR", "TS", "UK", "UA", "UP", "WB"]
# busier states appear more often
STATE_W = [1 if s not in ("MH", "KA", "DL", "TN", "UP", "GJ", "HR", "RJ", "TS", "AP", "KL", "WB", "MP") else 4
           for s in STATES]
SERIES_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ"  # I and O are avoided on real plates (still rare-sampled below)

# (background, text colour) per plate category
STYLES = [
    ((245, 245, 245), (15, 15, 15), 0.50),   # private: white / black
    ((255, 208, 20), (15, 15, 15), 0.30),    # commercial: yellow / black
    ((20, 130, 60), (250, 250, 250), 0.07),  # EV private: green / white
    ((20, 130, 60), (255, 210, 40), 0.04),   # EV commercial: green / yellow
    ((20, 20, 20), (255, 210, 40), 0.04),    # rental: black / yellow
    ((235, 235, 235), (160, 20, 20), 0.05),  # odd/old: white / red
]

_fonts = None
_font_cache = {}


def _font(path, size):
    if (path, size) not in _font_cache:
        _font_cache[(path, size)] = ImageFont.truetype(path, size)
    return _font_cache[(path, size)]


def fonts(font_dir):
    global _fonts
    if _fonts is None:
        _fonts = sorted(glob.glob(os.path.join(font_dir, "*.ttf")))
        assert _fonts, f"no fonts in {font_dir}"
    return _fonts


def random_plate():
    """Returns (top_row, bottom_row, full_text). Single-row plates use full_text."""
    if random.random() < 0.07:  # Bharat series: YY BH #### XX
        yy = f"{random.randint(21, 26):02d}"
        num = f"{random.randint(0, 9999):04d}"
        tail = "".join(random.choices(SERIES_LETTERS, k=random.choice((1, 2))))
        return yy + "BH", num + tail, yy + "BH" + num + tail
    st = random.choices(STATES, STATE_W)[0]
    if st == "DL" and random.random() < 0.6:
        dist = str(random.randint(1, 13))
    else:
        dist = f"{random.randint(1, 99):02d}"
    nser = random.choices((0, 1, 2, 3), (0.05, 0.25, 0.65, 0.05))[0]
    letters = SERIES_LETTERS if random.random() > 0.03 else SERIES_LETTERS + "IO"
    ser = "".join(random.choices(letters, k=nser))
    ndig = random.choices((4, 3, 2, 1), (0.85, 0.07, 0.05, 0.03))[0]
    num = str(random.randint(0, 10 ** ndig - 1)).zfill(4 if random.random() < 0.8 else ndig)
    return st + dist, ser + num, st + dist + ser + num


def _pick_style():
    r, acc = random.random(), 0.0
    for bg, fg, p in STYLES:
        acc += p
        if r <= acc:
            return bg, fg
    return STYLES[0][:2]


def _text_size(draw, text, font):
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    return r - l, b - t, l, t


def _render_row(draw, text, font, box, fg, spacing):
    """Draw text centred in box=(x0,y0,x1,y1) with optional group gaps."""
    x0, y0, x1, y1 = box
    widths = [_text_size(draw, c, font)[0] for c in text]
    total = sum(widths) + spacing * (len(text) - 1)
    # optional visual gaps between groups (e.g. "MH 43 CE 8461")
    gaps = [0] * len(text)
    if random.random() < 0.5:
        for i in range(1, len(text)):
            if text[i - 1].isalpha() != text[i].isalpha():
                gaps[i] = int(font.size * random.uniform(0.15, 0.45))
    total += sum(gaps)
    _, th, _, top = _text_size(draw, text, font)
    x = x0 + (x1 - x0 - total) / 2
    y = y0 + (y1 - y0 - th) / 2 - top
    for c, w, g in zip(text, widths, gaps):
        x += g
        draw.text((x, y), c, font=font, fill=fg)
        x += w + spacing


def render_plate(font_dir, two_row=None):
    """Render a clean plate. Returns (RGB image, [(row_text, row_box), ...], full_text)."""
    top, bottom, full = random_plate()
    if two_row is None:
        two_row = random.random() < 0.25
    bg, fg = _pick_style()
    font_path = random.choice(fonts(font_dir))
    fsize = 40
    font = _font(font_path, fsize)
    tmp = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    spacing = random.randint(1, 6)
    ind = random.random() < 0.5  # HSRP "IND" strip on the left
    ind_w = int(fsize * 0.55) if ind else 0
    pad = int(fsize * random.uniform(0.15, 0.35))
    if two_row:
        rw = max(_text_size(tmp, top, font)[0], _text_size(tmp, bottom, font)[0]) + spacing * 6
        pw, ph = rw + 2 * pad + ind_w, int(fsize * 2.5)
    else:
        rw = _text_size(tmp, full, font)[0] + spacing * len(full) + int(fsize * 0.6)
        pw, ph = rw + 2 * pad + ind_w, int(fsize * 1.35)
    img = Image.new("RGB", (pw, ph), bg)
    d = ImageDraw.Draw(img)
    bw = random.randint(2, 5)
    d.rectangle([bw // 2, bw // 2, pw - 1 - bw // 2, ph - 1 - bw // 2], outline=fg, width=bw)
    if ind:
        strip = (20, 60, 160) if random.random() < 0.6 else tuple(int(c * 0.85) for c in bg)
        d.rectangle([bw + 2, bw + 2, bw + 2 + ind_w - 6, ph - bw - 3], fill=strip)
        small = _font(font_path, max(9, fsize // 4))
        d.text((bw + 5, ph - bw - fsize // 3 - 4), "IND", font=small, fill=(240, 240, 240))
        d.ellipse([bw + 6, bw + 8, bw + ind_w - 6, bw + ind_w - 4], outline=(200, 200, 210), width=2)
    x0 = bw + ind_w + pad // 2
    rows = []
    if two_row:
        mid = ph // 2
        _render_row(d, top, font, (x0, bw, pw - bw - pad // 2, mid), fg, spacing)
        _render_row(d, bottom, font, (x0, mid, pw - bw - pad // 2, ph - bw), fg, spacing)
        rows = [(top, "top"), (bottom, "bottom")]
    else:
        _render_row(d, full, font, (x0, bw, pw - bw - pad // 2, ph - bw), fg, spacing)
        rows = [(full, "full")]
    return np.array(img), rows, full


def _perspective(img, mag):
    h, w = img.shape[:2]
    src = np.float32([[0, 0], [w, 0], [w, h], [0, h]])
    j = lambda s: np.random.uniform(-mag, mag) * s
    dst = np.float32([[j(w), j(h)], [w + j(w), j(h)], [w + j(w), h + j(h)], [j(w), h + j(h)]])
    m = cv2.getPerspectiveTransform(src, dst)
    return m


def _background(h, w):
    r = random.random()
    if r < 0.4:
        c = np.random.randint(0, 255, 3)
        bg = np.full((h, w, 3), c, np.uint8)
    elif r < 0.7:
        bg = np.random.randint(0, 255, (h // 8 + 1, w // 8 + 1, 3), np.uint8)
        bg = cv2.resize(bg, (w, h), interpolation=cv2.INTER_CUBIC)
    else:
        bg = np.full((h, w, 3), np.random.randint(60, 200), np.uint8)
        n = np.random.randint(0, 40, (h // 4 + 1, w // 4 + 1, 3), dtype=np.uint8)
        bg = cv2.add(bg, cv2.resize(n, (w, h), interpolation=cv2.INTER_NEAREST))
    return bg


def _degrade(img):
    h, w = img.shape[:2]
    # low resolution: the plate is often only 40-100 px wide in the 960x540 stream
    target_w = int(np.random.choice([random.randint(36, 70), random.randint(70, 130), random.randint(130, 220)],
                                    p=[0.3, 0.5, 0.2]))
    s = target_w / w
    small = cv2.resize(img, (max(8, int(w * s)), max(4, int(h * s))), interpolation=cv2.INTER_AREA)
    if random.random() < 0.5:
        k = random.choice((3, 5))
        if random.random() < 0.5:
            small = cv2.GaussianBlur(small, (k, k), 0)
        else:  # motion blur
            m = np.zeros((k, k), np.float32)
            m[k // 2, :] = 1.0 / k
            small = cv2.filter2D(small, -1, m)
    # lighting: brightness/contrast/gamma, colour cast, shadow or glare band
    a = random.uniform(0.55, 1.35)
    b = random.uniform(-45, 45)
    small = np.clip(small.astype(np.float32) * a + b, 0, 255)
    small *= np.random.uniform(0.85, 1.15, 3)
    if random.random() < 0.35:
        gh, gw = small.shape[:2]
        grad = np.tile(np.linspace(random.uniform(0.4, 1.0), random.uniform(0.9, 1.4), gw), (gh, 1))
        if random.random() < 0.5:
            grad = grad[:, ::-1]
        small *= grad[..., None]
    small = np.clip(small, 0, 255)
    if random.random() < 0.6:
        small += np.random.normal(0, random.uniform(2, 12), small.shape)
    small = np.clip(small, 0, 255).astype(np.uint8)
    # NV12 chroma subsampling like the device (only works on even sizes)
    sh, sw = small.shape[:2]
    if sh >= 4 and sw >= 4:
        e = small[: sh & ~1, : sw & ~1]
        yuv = cv2.cvtColor(e, cv2.COLOR_RGB2YUV_I420)
        small[: sh & ~1, : sw & ~1] = cv2.cvtColor(yuv, cv2.COLOR_YUV2RGB_I420)
    if random.random() < 0.7:
        q = random.randint(25, 90)
        ok, enc = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, q])
        small = cv2.imdecode(enc, cv2.IMREAD_UNCHANGED)
    return small


def make_sample(font_dir):
    """One training sample: (HxWx3 uint8 RGB at 32x128, label string)."""
    plate, rows, _ = render_plate(font_dir)
    ph, pw = plate.shape[:2]
    # place on a background with margin, apply perspective + small rotation
    m = int(max(ph, pw) * 0.35)
    canvas = _background(ph + 2 * m, pw + 2 * m)
    canvas[m:m + ph, m:m + pw] = plate
    M = _perspective(canvas, 0.04)
    ang = random.uniform(-6, 6)
    R = cv2.getRotationMatrix2D((canvas.shape[1] / 2, canvas.shape[0] / 2), ang, 1.0)
    R3 = np.vstack([R, [0, 0, 1]])
    T = R3 @ M
    warped = cv2.warpPerspective(canvas, T, (canvas.shape[1], canvas.shape[0]), borderMode=cv2.BORDER_REPLICATE)
    corners = np.float32([[m, m], [m + pw, m], [m + pw, m + ph], [m, m + ph]]).reshape(-1, 1, 2)
    pc = cv2.perspectiveTransform(corners, T).reshape(-1, 2)
    x1, y1 = pc.min(0)
    x2, y2 = pc.max(0)
    # detector box jitter, then the device's expansion (6% x, 12% y)
    bw_, bh_ = x2 - x1, y2 - y1
    jx, jy = 0.05 * bw_, 0.08 * bh_
    x1 += random.uniform(-jx, jx); x2 += random.uniform(-jx, jx)
    y1 += random.uniform(-jy, jy); y2 += random.uniform(-jy, jy)
    bw_, bh_ = x2 - x1, y2 - y1
    x1, x2 = x1 - 0.06 * bw_, x2 + 0.06 * bw_
    y1, y2 = y1 - 0.12 * bh_, y2 + 0.12 * bh_

    text, which = random.choice(rows)
    if which == "top":
        y2 = (y1 + y2) / 2 + 0.04 * (y2 - y1)
    elif which == "bottom":
        y1 = (y1 + y2) / 2 - 0.04 * (y2 - y1)
    hh, ww = warped.shape[:2]
    x1, y1 = int(max(0, x1)), int(max(0, y1))
    x2, y2 = int(min(ww, x2)), int(min(hh, y2))
    crop = warped[y1:y2, x1:x2]
    crop = _degrade(crop)
    crop = cv2.resize(crop, (W, H), interpolation=cv2.INTER_LINEAR)
    return crop, text


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "samples.jpg"
    fd = sys.argv[2] if len(sys.argv) > 2 else "fonts"
    tiles = []
    for _ in range(48):
        im, t = make_sample(fd)
        im = cv2.resize(im, (256, 64))
        cv2.putText(im, t, (2, 62), cv2.FONT_HERSHEY_PLAIN, 0.9, (255, 0, 255), 1)
        tiles.append(im)
    grid = np.vstack([np.hstack(tiles[i:i + 4]) for i in range(0, 48, 4)])
    cv2.imwrite(out, grid[:, :, ::-1])
    print("wrote", out)
