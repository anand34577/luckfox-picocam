# PicoCam for Luckfox Pico

Open-source IP-camera firmware for the **Luckfox Pico family** (Rockchip RV1103 / RV1106). It replaces the vendor `rkipc` demo
with one small native daemon and a modern web portal: live view, RTSP/ONVIF, recording, motion and on-board AI (people, vehicles,
plates, faces) on the NPU, webhooks / MQTT / Telegram / email, and a self-healing updater.

## Install in three steps

1. **Flash the stock Luckfox Buildroot image** for your board (see the Luckfox wiki) and plug the board in. Over USB it has the
   address `172.32.0.93`; boards with Ethernet also get one from your router.
2. **Run the installer from your PC** (needs only an SSH client; the default login is `root` / `luckfox`):

   | Windows (PowerShell) | Linux / macOS / WSL / Git Bash |
   |---|---|
   | `.\picocam\tools\deploy.ps1` | `picocam/tools/deploy.sh` |
   | `.\picocam\tools\deploy.ps1 -Board 192.168.1.64` | `picocam/tools/deploy.sh -b 192.168.1.64` |
   | `.\picocam\tools\deploy.ps1 -Lite` | `picocam/tools/deploy.sh --lite` |

   It downloads the newest release, uploads it over SSH and installs it. Use `-Lite` / `--lite` (no demo models) on boards with
   little flash.
3. **Open `http://<camera-ip>/`**, set the admin password and follow the setup wizard.

To compile it yourself see [docs/BUILDING.md](docs/BUILDING.md). Back to the stock firmware at any time:
`sh /oem/picocam/uninstall.sh && reboot`.

## Which boards?

One package serves every board. While running, the firmware detects the chip, RAM, boot flash (SPI NAND / eMMC / SD),
Ethernet/Wi-Fi and the camera module, shows them under **System**, and sets its limits from them. Details, flash behaviour and what
is verified: **[docs/BOARDS.md](docs/BOARDS.md)**.

| Family | Chip | Notes |
|---|---|---|
| Pico, Pico Mini, Pico Plus ... | RV1103 | 64-128 MB RAM; Ethernet on the Plus; SPI NAND or SD |
| Pico Pro, Pico Max, Pico Ultra ... | RV1106 | 256 MB RAM; Ethernet; SPI NAND / eMMC / SD |

Tested on hardware by the maintainers so far: **Luckfox Pico** and **Pico Plus** with the SC3336 3 MP module. Other boards use the
same code paths but are **not verified on hardware**; please file a *Board report* issue.

## What it does

Video (H.264/H.265 up to the sensor's full resolution), Smart Exposure, RTSP + ONVIF Profile S, browser live view (about 0.3 s),
SD recording with timeline, motion detection, NPU AI pipelines with your own `.rknn` models (YOLOv5/8/11, RetinaFace, OCR,
classifiers), rules (zones, line crossing, counting, ANPR logs), HTTPS, users and roles, audit log, hardware watchdog and
automatic update rollback. Feature list and API: [picocam/README.md](picocam/README.md).

## Repository map

| Path | What |
|---|---|
| `picocam/` | firmware (C++), web portal (Preact), board scripts, installers |
| `docs/` | boards, building, troubleshooting |
| `models/`, `ocr/` | scripts to train and convert AI models (ONNX to RKNN) |
| `server/`, `web/` | optional EdgeAI receiver (Go) and its dashboard for collecting events from many cameras |
| `docker/` | the build/conversion image (cross compiler, RKNN-Toolkit2) |

## Contributing and licence

Contributions are welcome: [CONTRIBUTING.md](CONTRIBUTING.md). The code is MIT licensed ([LICENSE](LICENSE)); bundled third-party
components and the licence caveats for the demo AI models are in [NOTICE.md](NOTICE.md).
