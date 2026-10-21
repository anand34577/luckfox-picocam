#!/usr/bin/env python3
"""Build a PicoCam model package (one .tar.gz = model + everything the camera needs to understand it).

  python3 models/package.py yolox_s.rknn --name "YOLOX-S" --task detect --labels labels.txt --decoder yolox -o yolox_s.tar.gz
  python3 models/package.py plate_ocr.rknn --task ocr --charset 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ --layout tc --blank first
  python3 models/package.py custom.rknn --task detect --decoder plugin --plugin my_decoder.so   # plugin = admin upload only

Upload it in AI > Models. Fields mirror the model's sidecar json (see the README, "AI: bring your own model").
"""
import argparse, io, json, os, sys, tarfile

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('rknn')
ap.add_argument('-o', '--out')
ap.add_argument('--name'); ap.add_argument('--description')
ap.add_argument('--task', choices=['detect', 'face_detect', 'classify', 'ocr', 'face_embed'])
ap.add_argument('--labels', help='text file, one class per line')
ap.add_argument('--color', choices=['rgb', 'bgr'])
ap.add_argument('--decoder', choices=['auto', 'yolov5', 'yolov8', 'yolox', 'yolov10', 'plugin'])
ap.add_argument('--anchors', help='yolov5/v7 custom anchors: 18 numbers, comma separated (3 heads x 3 anchors x w,h)')
ap.add_argument('--stretch', action='store_true', help='resize without letterbox padding')
ap.add_argument('--class-agnostic', action='store_true')
ap.add_argument('--charset'); ap.add_argument('--layout', choices=['ct', 'tc'], help='ocr output: classes x steps (ct) or steps x classes (tc)')
ap.add_argument('--blank', help='ocr CTC blank: last (default), first, or an index')
ap.add_argument('--center-crop', action='store_true', help='classifier: central square instead of stretching')
ap.add_argument('--plugin', help='decoder plugin (.so built with the camera toolchain)')
a = ap.parse_args()

m = {k: v for k, v in dict(name=a.name, description=a.description, task=a.task, color=a.color, decoder=a.decoder,
                           charset=a.charset, layout=a.layout, blank=a.blank).items() if v}
if a.anchors:
    nums = [float(x) for x in a.anchors.split(',')]
    if len(nums) != 18: sys.exit('--anchors needs 18 numbers')
    m['anchors'] = nums
if a.stretch: m['stretch'] = True
if a.class_agnostic: m['class_agnostic'] = True
if a.center_crop: m['center_crop'] = True
if a.plugin: m['decoder'] = 'plugin'; m['plugin'] = os.path.basename(a.plugin)
out = a.out or os.path.splitext(a.rknn)[0] + '.tar.gz'
with tarfile.open(out, 'w:gz') as t:
    t.add(a.rknn, arcname=os.path.basename(a.rknn))
    data = json.dumps(m, indent=2).encode()
    ti = tarfile.TarInfo('manifest.json'); ti.size = len(data); t.addfile(ti, io.BytesIO(data))
    if a.labels: t.add(a.labels, arcname='labels.txt')
    if a.plugin: t.add(a.plugin, arcname=os.path.basename(a.plugin))
print('wrote', out, '-', json.dumps(m))
