# AI models

The camera runs AI on its built-in NPU. The AI itself comes as **model files** (`.rknn`). PicoCam includes five ready-made models so
you can try everything straight away, and you can add your own.

## The ready-made models

They are in `picocam/models/` and installed with the full package (not with `--lite`).

| Model | What it does | Used by template |
|---|---|---|
| Objects (COCO 80, YOLOv5n, 320) | Finds 80 kinds of objects: people, cars, buses, trucks, bikes, animals ... | People & vehicles, ANPR, People counting |
| Number plate detector (YOLO11n, 320) | Finds the plate on a vehicle | ANPR |
| Indian plate reader (PlateNet OCR) | Reads single and two-row Indian plates | ANPR |
| Face detector (RetinaFace, 320) | Finds faces and five landmarks | Face capture |
| Face embedding (MobileFaceNet) | Turns a face into numbers so people can be recognised | Face capture |

**To use them:** open *AI & Analytics*, click a template (for example *People & vehicle detection*) and *Apply*. That is all.

> **Important: models are built for one chip.** Rockchip compiles a model for a specific chip. The ready-made models are built for
> **RV1103**. On an RV1106 board they may refuse to load; the page then tells you which chip your board is, and you convert the model
> again for it (below). *System > Overview* shows your chip. This is the single most common reason for "model not valid".

## Getting the models onto the camera

| Situation | Do this |
|---|---|
| Normal install | Already there |
| You installed the lite package | `picocam/tools/deploy.sh --models` (Windows: `deploy.ps1 -Models`). It adds them to `/userdata/picocam/models` |
| Only one model | *AI & Analytics > Models > Upload*, choose the `.rknn` file (and a class list), or a model package |
| Offline | Download `picocam-models.tar.gz` from the release page and use `deploy.sh -p picocam-models.tar.gz --models` |

Models take space: about 12 MB for all five. On SPI NAND boards with a small user partition the script checks first and tells you if
it will not fit. A card is not needed for models, but an SD-booted board has plenty of room.

## Use your own model

1. **Train** with Ultralytics YOLOv5, YOLOv8 or YOLO11, PyTorch or Paddle, and export to **ONNX**. For YOLOv8/11 use
   `models/export_yolov8_rknn.py`: it produces the layout the NPU likes.
2. **Convert** to RKNN with the project's tools (the Docker image in `docker/` contains RKNN-Toolkit2):

   ```sh
   python3 models/convert.py model.onnx calib.txt model.rknn --size 320 --preset yolo --platform rv1103   # or rv1106
   ```

   `calib.txt` lists 20-100 pictures **from your own camera**, one path per line. They are what makes the 8-bit conversion accurate.
   Do not publish them: real footage shows people and number plates.
3. **Package** (optional but tidy): `python3 models/package.py model.rknn --decoder yolox --labels labels.txt` makes one `.tar.gz` that
   carries the settings the camera needs.
4. **Upload** under *AI & Analytics > Models*. The task (detector, classifier, text reader, face embedding) and the tensor shapes
   are recognised automatically. **Test on live picture** shows the result at once, and **Benchmark** measures speed on your NPU.
5. **Use it** in *Pipeline* as the detector, or as a stage that runs on each found object (for example *vehicle, then plate, then text*).

Rules for models: 8-bit input, normalisation built in (`--preset`), no more than 16 MB on 64 MB boards and 40 MB on larger ones.

Supported outputs: YOLOv5 (also custom anchors), YOLOv8/YOLO11 (Rockchip layout), YOLOX, YOLOv10, RetinaFace, classifiers, CTC text
readers, embeddings. Anything else can be added with a small **decoder plugin** (`picocam/sdk/`), administrators only.

## Licences

The ready-made models are conveniences with their own licences: some are derived from Ultralytics YOLO (AGPL-3.0) and public
face-model architectures. They are **not** covered by the project's MIT licence. If you build a product, check [NOTICE](../NOTICE.md)
or use the lite package with models you have the rights to.

## How well does it work?

An honest rule of thumb on these small chips: people and vehicles at a normal gate distance work well; plates need a plate that is
roughly 100 pixels wide or more in the analysed frame (raise the sub-stream resolution or use the *detection area* to zoom into the
road) and enough shutter speed at night (*Image > Scene > Road / ANPR* does this). Faces work best in even light at close range.
