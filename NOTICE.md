# Third-party components

| Component | Licence | Where |
|---|---|---|
| Mbed TLS | Apache-2.0 | `picocam/firmware/third_party/mbedtls` |
| nlohmann/json | MIT | `picocam/firmware/third_party/json.hpp` |
| stb_image_write | public domain / MIT | `picocam/firmware/third_party/stb_image_write.h` |
| Mozilla CA bundle | MPL-2.0 | `picocam/board/ca-bundle.pem` |
| Rockchip RKNN runtime `librknnmrt.so` | Rockchip licence (redistributed unmodified from `airockchip/rknn-toolkit2`) | package `lib/` |
| Rockchip RKMPI / rkaiq libraries | provided by the Luckfox image on the board, not redistributed | |
| Preact | MIT | web UI |
| Photo "Tram riding along wet city street near modern cars" by Vlad Fonsark ([Pexels](https://www.pexels.com/photo/tram-riding-along-wet-city-street-near-modern-cars-4993460/)) | Pexels licence; number plates covered with fictional ones | `picocam/web/mock/street.jpg` (demo scene and screenshots only) |

## Bundled demo AI models (`picocam/models`)

The demo models are convenience downloads and are **not** covered by the MIT licence of this repository's code. Some derive from
Ultralytics YOLO (AGPL-3.0), RetinaFace and MobileFaceNet architectures and from data with their own terms. If you redistribute a
product built on them, check those licences, or ship the `-lite` package with your own models.
