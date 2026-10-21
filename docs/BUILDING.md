# Building from source

You need a PC with Docker (Linux, macOS, or Windows with WSL2) and Node 18+. The container holds the Luckfox uClibc cross compiler,
the RKMPI/RKNN SDK headers and RKNN-Toolkit2.

```sh
docker build -t luckfox-ai:1.1 docker/                       # once, large image
docker run --rm -v "$PWD":/work -w /work/picocam luckfox-ai:1.1 sh build.sh firmware
(cd picocam/web && npm ci && npm run build)
docker run --rm -v "$PWD":/work -w /work/picocam luckfox-ai:1.1 bash tools/package.sh
```

Results: `picocam/dist/picocam-<version>.tar.gz` (with demo models) and `picocam-<version>-lite.tar.gz`. Install with
`picocam/tools/deploy.sh -p picocam/dist/picocam-<version>.tar.gz`. `deploy.sh --build` and `deploy.ps1 -Build` do all of it.

## Tests

```sh
docker run --rm -v "$PWD":/work -w /work/picocam/firmware luckfox-ai:1.1 sh tests/run.sh
```

Host-built checks of TLS, HMAC, the tracker, plate voting, the face gallery and the IP allow-list. The web UI is checked with
`cd picocam/web && npx tsc --noEmit`.

## Your own calibration images

INT8 conversion needs 20-100 frames from *your* camera. Put them in `models/calib/` and list them in `models/calib.txt`, one path
per line. They are git-ignored on purpose: real footage contains people and number plates.

## Converting a model

```sh
python3 models/convert.py model.onnx models/calib.txt model.rknn --size 320 --preset yolo --platform rv1103   # or rv1106
python3 models/package.py model.rknn --decoder yolox --labels labels.txt                                    # one upload file
```
