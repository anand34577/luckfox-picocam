# PicoCam — IP camera firmware for the Luckfox Pico family

> **1.2.0** — runs on the whole RV1103/RV1106 Pico family (auto-detected), SPI-NAND aware, USB address settable in the UI; see [../CHANGELOG.md](../CHANGELOG.md).
> reliable delivery queue with retry, Telegram / email / FTP / GPIO actions, hardware watchdog with automatic update rollback,
> scene presets with AI-aware exposure and auto day/night, ROI/tiled detection, decoder plugins, model packages, on-device face
> recognition, a rebuilt tracker, security page with audit log, setup wizard and a phone-friendly portal.

PicoCam turns a Luckfox Pico board (RV1103/RV1106, SC3336 3 MP or another supported module) into a full IP camera. It replaces the vendor
`rkipc` app with one native daemon (`picocam`, about 1.5 MB, 9 MB RAM) and adds a modern web portal.

| Area | What you get |
|---|---|
| Video | Main stream up to 2304×1296 at 25 fps and a sub stream up to 1280×720. H.264 or H.265, CBR/VBR/AVBR, GOP, quality and SmartP. OSD with time and name, up to 8 privacy masks, full-resolution JPEG snapshots |
| Image | **Smart Exposure** (histogram-driven: protects sky and lamps, lifts backlit subjects, automatic anti-flicker), auto WDR, BLC/HLC, defog, 2D/3D noise reduction, white balance presets and manual, mirror/flip, B/W mode, metering modes, live histogram |
| Live view | Browser live view: WebCodecs on HTTPS, Media Source Extensions on plain HTTP, about 0.2–0.3 s delay. AI boxes and zones drawn on top, snapshot, fullscreen |
| Streaming | RTSP with Digest/Basic auth, TCP and UDP, Hikvision/Dahua-style URL aliases. Slow viewers skip to the next keyframe instead of lagging |
| NVR / VMS | ONVIF Profile S (device, media, imaging, events PullPoint with motion alarms) and WS-Discovery |
| AI (NPU) | Upload your own `.rknn` models. Configurable pipeline: detector (YOLOv5/v8/11, RetinaFace), then per-object stages (part detection, OCR, classification, face embedding), tracking and voting. Ready-made templates: people/vehicles, ANPR (Indian plates), face capture, people counting |
| Rules | Intrusion zones, loitering, line crossing with counters, crowd count, presence, per-object records (ANPR logs), attribute lists (plate allow/deny) |
| Motion | Pixel-based motion detection (no NPU needed) with zones, sensitivity and cooldown |
| Events | Snapshot and crops per event, searchable gallery, CSV export, realtime push to the UI |
| Integrations | Webhooks (HTTP/HTTPS, HMAC-signed), MQTT (TLS, QoS 1, Home Assistant discovery), Telegram, email (SMTP), FTP, GPIO relay/lamp outputs, your EdgeAI server, ONVIF alarms, REST API with API keys, WebSocket event stream. Every target has its own persistent retry queue |
| Recording | SD card: continuous, schedule (weekly grid) or event recording with pre/post buffer. Fragmented MP4 (power-loss safe), overwrite oldest, timeline playback and download in the browser |
| Security | HTTPS with self-signed or your own certificate, IP allow-list, session list/revoke, audit log, SSH/ADB switches, root password tied to the admin password, hardware watchdog, firmware rollback and last-known-good configuration, setup wizard |
| System | First-run activation, users (admin/operator/viewer), login lockout, firmware update from the browser (previous version kept), config backup/restore, NTP and time zone, network settings (Pico Plus Ethernet), logs, health dashboard, media-memory size |

## Install

From a PC with the board reachable over SSH (USB: `172.32.0.93`; Ethernet boards: the address from your router). Default login
`root` / `luckfox`.

```sh
tools/deploy.sh                      # Linux / macOS / WSL / Git Bash: downloads the newest release and installs it
tools/deploy.sh -b 192.168.1.64 --lite
```
```powershell
.	ools\deploy.ps1                   # Windows; -Board <ip>, -Lite, -Package <file>, -Build
```

By hand, with a package `picocam-x.y.z.tar.gz` (the board's ssh server needs no scp):

```sh
ssh root@172.32.0.93 'mkdir -p /userdata/pcinst && gunzip -c | tar -x -C /userdata/pcinst' < picocam-1.2.0.tar.gz
ssh root@172.32.0.93 'sh /userdata/pcinst/install.sh'      # add --no-models on boards with little flash
```

Then open `http://<camera-ip>/` and set the admin password. Later updates can be installed from **System > Maintenance**.

The installer checks the board and free space first. It installs into `/oem/picocam` (data in `/userdata/picocam`), adds
`/etc/init.d/S95picocam` (a 3-line stub that runs `/oem/picocam/service.sh`, so updates can improve the supervisor too; if `/etc`
is read-only the launcher hook starts it instead), and stops `rkipc` from starting. After an update the new version is on
probation for 2 minutes: if it keeps crashing the camera goes back to the previous version, and a configuration that keeps
crashing it is replaced by the last one that ran well.

**Back to stock:** `sh /oem/picocam/uninstall.sh` and reboot (`--purge` also deletes the settings).
Board support, flash behaviour and USB networking: [../docs/BOARDS.md](../docs/BOARDS.md).

## Addresses

| | |
|---|---|
| Web portal | `http://<ip>/` |
| RTSP main / sub | `rtsp://<ip>/live/main`, `rtsp://<ip>/live/sub`. Aliases: `/Streaming/Channels/101` and `102`, `/h264/ch1/main/av_stream`, `/cam/realmonitor?subtype=0` and `subtype=1`, `/live/0` and `1` |
| ONVIF | `http://<ip>/onvif/device_service` (discoverable) |
| Snapshot | `http://<ip>/snapshot.jpg` (Basic/Digest auth, or `?token=pk_…`) |
| Events WebSocket | `ws://<ip>/ws/events` (topics `event`, `detections`, `motion`, `status`) |

Accounts work everywhere (web, RTSP, ONVIF). Viewers can watch and play back. Operators can also change settings and AI. Admins can do everything.

## Latency

- **Web live view:** about 0.2–0.3 s on a LAN. The player stays pinned to the live edge.
- **VLC:** default network caching is 1000 ms. Set *Tools › Preferences › Input/Codecs › Network caching* to 150–300 ms.
- **ffplay:** `ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://user:pass@<ip>/live/sub`
- **GOP:** shorter keyframe intervals (GOP) make streams start faster.

## Smart Exposure

The Rockchip AE drives the *average* brightness to a setpoint. That over-exposes a road in front of a bright sky and under-exposes a backlit gate. Smart Exposure runs on top of it four times a second, using the sub-stream histogram:

- **Mid-tones:** the median is driven to the *Target brightness*.
- **Highlights:** they stay unclipped, unless that would crush most of the picture (backlight). In that case it keeps the subject exposed and raises the DRC (digital WDR) gain to recover shadows.
- **Anti-flicker *Auto*:** anti-flicker forces exposures in steps of 10 ms. That was the cause of the original "white in sunlight" problem. Auto only engages it when the scene is dim enough to need 10 ms or more anyway (indoor mains lighting), and drops it in daylight.

The **Image** page shows shutter, gain, bias, WDR gain, clipping and the histogram live.

## AI: bring your own model

1. Train (Ultralytics YOLOv5/v8/11, PyTorch, Paddle) and export ONNX.
   - For YOLOv8/11 use `models/export_yolov8_rknn.py`. It produces the Rockchip-friendly DFL head layout.
2. Convert to RKNN INT8 for the RV1103/RV1106. The `luckfox-ai` Docker image has RKNN-Toolkit2:
   ```sh
   python3 models/convert.py model.onnx calib.txt model.rknn --size 320 --preset yolo
   ```
   `calib.txt` lists 20–100 frames from *this* camera.
3. Upload it in **AI › Models** with the class labels, one per line. The task (detector, classifier, OCR, embedding) and tensor shapes are detected automatically. Use **Test on live picture** to check it.
4. Use it in **AI › Pipeline**, either as the detector or as a per-object stage.
   - Stages chain: for example *vehicle → plate detector → plate OCR*, or *person → helmet classifier*.
   - Results become event attributes (`plate_text`, `helmet`, …) that rules can filter.

Supported outputs:
- **YOLOv5:** 3 anchor heads.
- **YOLOv8 / YOLO11:** Rockchip DFL export with 6 or 9 outputs.
- **RetinaFace.**
- **Classifiers:** logits or probabilities.
- **CTC text recognisers:** charset in model order, blank last.
- **Embedding models.**

Models must take uint8 NHWC input, with normalisation built in through `mean/std` in the conversion.

### Media memory

The ISP, encoders and NPU share a fixed media-memory pool (CMA), 24 MB on stock images. PicoCam allocates in an order that lets the encoders and the NPU both get contiguous blocks. If a pipeline doesn't fit, the camera keeps streaming and the AI page says why.

| Media memory | Typical fit |
|---|---|
| 24 MB (stock) | 3 MP main stream plus one detector (people/vehicles) |
| 28 MB | 1080p plus full ANPR (3 models), or 3 MP plus a detector with headroom |
| 32 MB | 3 MP plus full ANPR or face pipelines |

Change it in **System › Performance** (SD-booted boards). It is stored in the U-Boot environment (CRC-checked, with a backup in `/userdata/picocam/env.backup`) and needs a reboot. Every MB taken from media memory is taken from system RAM.

## Integrations

**Webhook payload** (`POST`, JSON):

```json
{"event": {"id": "20261021-190555-8f10", "ts": 1792589755683, "time": "2026-10-21T19:05:55",
           "type": "track", "rule": "Vehicle log", "label": "car", "track_id": 12, "score": 0.81,
           "box": [0.31, 0.42, 0.58, 0.77], "attributes": {"plate_text": "MH12AB1234"},
           "images": {"snapshot": "…jpg", "plate": "…jpg"}},
 "device": {"id": "picocam-743224", "name": "Gate", "location": "", "ip": "192.168.1.64"},
 "images": {"snapshot": "<base64>"}}
```

**Event pictures:** `GET /api/events/<id>/<image>.jpg`.

**MQTT topics:**
- `<topic>/events` (full event)
- `<topic>/events/<type>`
- `<topic>/state`: `online`/`offline`, retained, with last will

**EdgeAI server:** events are sent as multipart (`meta`, `frame`, `crop_*`) to `/api/v1/events`, with heartbeats to `/api/v1/heartbeat`. The server's clock also sets the camera's clock.

**REST API:**
- **Auth:** an API key from **Integrations › API & keys** (`Authorization: Bearer pk_…`), or Basic/Digest.
- **Common endpoints:**
  - `GET /api/status`
  - `GET /api/events?type=&label=&q=&date=&limit=`
  - `GET /api/events/export.csv`
  - `GET /api/snapshot.jpg`
  - `GET|PUT /api/config/<section>`
  - `GET /api/recordings?date=YYYY-MM-DD`
  - `GET /api/recordings/file/<day>/<file>` (Range supported)
  - `POST /api/ai/models?filename=…` (raw `.rknn` body)
  - `POST /api/ai/test`

## Building

Requirements: Docker with the `luckfox-ai:1.1` image (`docker/Dockerfile`), plus Node 18+. Full steps: [../docs/BUILDING.md](../docs/BUILDING.md).

```sh
# firmware (inside WSL)
docker run --rm -v "$PWD":/work -w /work/picocam luckfox-ai:1.1 sh build.sh firmware
# web UI
cd picocam/web && npm ci && npm run build
```

`tools/deploy.sh --build` / `deploy.ps1 -Build` do both, package the result and install it on a board.

Source layout:
- `firmware/src`
  - `core/`: config, logging, utils
  - `media/`: ISP and Smart Exposure, video pipeline, MP4
  - `net/`: event loop, HTTP/WS, RTSP, ONVIF, MQTT
  - `ai/`: NPU, detectors, tracker, engine, rules, motion
  - `app/`: API, auth, events, storage, system
- `web/`: Preact + TypeScript UI
- `board/`: init script, installer

## Troubleshooting

| Symptom | Fix |
|---|---|
| No video after a settings change | Check **AI** for a media-memory message. Lower the main resolution or raise media memory |
| RTSP port busy after manually killing `rkipc` | `rkipc` leaks its socket into `udhcpc`. Reboot once (PicoCam starts before anything can take it) |
| Clock shows 1970 | No RTC and no internet. Log in as admin once (the browser sets the clock), use **Network & Time › Sync**, or point NTP at a LAN server |
| Live view says the browser can't play H.265 | Use H.264 for the sub stream, or a Chromium browser with hardware HEVC |
| Service logs | `ssh root@<ip> tail -f /tmp/picocam.log`, or **System › Logs** |


## Reliability, security and delivery (1.1)

- **Watchdog:** `/dev/watchdog` (60 s) is fed only while the event loop and the video encoder are alive; a hang resets the board. Switch it off in System › Security.
- **Delivery queue:** each stored event is queued per target (`/userdata/picocam/outbox`), retried with backoff for up to 24 h, survives reboots. Webhooks carry `X-PicoCam-Event-Id` (use it to drop duplicates) and, with a secret set, `X-PicoCam-Signature: sha256=HMAC(secret, timestamp + "." + body)`.
- **HTTPS:** a self-signed ECDSA certificate is created on first start (port 443); upload your own in System › Security. WebCodecs live view (lowest delay) needs HTTPS. Outgoing HTTPS/MQTT-TLS is verified against `ca-bundle.pem` (Mozilla roots), a pinned SHA-256 fingerprint, or skipped.
- **Plugins:** `sdk/pc_plugin.h` + `sdk/example_decoder.c` show how to decode any model's outputs; plugins are administrator-only.
- **Model packages:** `python3 models/package.py model.rknn --decoder yolox --labels labels.txt` builds one `.tar.gz` to upload.
- **Tests:** `docker run --rm -v "$PWD":/work -w /work/picocam/firmware luckfox-ai:1.1 sh tests/run.sh` (TLS handshake, HMAC vectors, tracker, plate voting, face gallery, IP filter).
