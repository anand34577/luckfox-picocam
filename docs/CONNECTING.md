# Using the camera with other software

PicoCam speaks the standard languages, so most programs work without special setup. Replace `<ip>` with the camera's address,
`user` / `password` with an account from *System > Users* (viewer is enough for watching).

## Addresses at a glance

| What | Address |
|---|---|
| Web portal | `http://<ip>/` (also `https://<ip>/`) |
| Video, full resolution | `rtsp://user:password@<ip>/live/main` |
| Video, light (sub stream) | `rtsp://user:password@<ip>/live/sub` |
| Still picture (JPEG) | `http://<ip>/snapshot.jpg` (login required) |
| ONVIF | `http://<ip>/onvif/device_service` (also found by automatic discovery) |
| Event stream (WebSocket) | `ws://<ip>/ws/events` |

Aliases for software that expects another brand's address: `/Streaming/Channels/101` and `102`, `/h264/ch1/main/av_stream`,
`/cam/realmonitor?channel=1&subtype=0` and `subtype=1`, `/live/0` and `/live/1`.

> **Special characters in the password** (`@ : / ? #`) must be written as `%40 %3A %2F %3F %23` inside a URL. Or pick a password
> without them for the camera accounts you use in URLs.

## Watching video

**VLC.** *Media > Open Network Stream*, paste the RTSP address. VLC buffers one second by default; for a snappier picture open
*Tools > Preferences > Show all > Input/Codecs > Network caching* and set **150-300 ms**.

**ffplay (lowest delay).**

```sh
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://user:password@<ip>/live/sub
```

**Phone.** Any RTSP viewer app. Or just use the web portal: it is built for phones.

**Choosing a stream.** *Main* is for recording and NVRs. *Sub* (960x540 by default) is for live viewing, phones and AI.
If a player cannot show H.265, set the stream to H.264 in *Video & Audio*.

## Network video recorders (NVR / VMS)

Add the camera as an **ONVIF** device (address `<ip>`, port `80`, an account from *System > Users*), or paste the RTSP address as a
generic camera. Works with Synology Surveillance Station, Blue Iris, Milestone, Hikvision/Dahua NVRs and similar. Motion alarms are
sent as ONVIF events.

## Frigate

```yaml
cameras:
  gate:
    ffmpeg:
      inputs:
        - path: rtsp://user:password@<ip>/live/main
          roles: [record]
        - path: rtsp://user:password@<ip>/live/sub
          roles: [detect]
    detect: { width: 960, height: 540, fps: 5 }
```

The camera can also do detection itself on its NPU; Frigate then only records. Use whichever you prefer.

## Home Assistant

- **Quickest:** *Settings > Devices > Add integration > ONVIF*, enter the camera's address, port 80 and an account.
- **With events:** in the camera under *Integrations > MQTT*, point it at your broker and switch on **Home Assistant discovery**. The camera
  then appears as a device with motion, person, last event, last plate and line-counter sensors.
- **Picture only:** the *Generic Camera* integration with still URL `http://<ip>/snapshot.jpg` (use an API key, see below) and stream URL
  `rtsp://user:password@<ip>/live/sub`.

## Getting alerts

| Where | How |
|---|---|
| **Telegram** | Create a bot with `@BotFather`, add it to your chat, paste its token and chat id under *Integrations > Telegram* |
| **Email** | *Integrations > Email (SMTP)*: server, port, account, recipient. Use an app password for Gmail |
| **Webhook** | *Integrations > Webhooks*: any `http(s)://` address gets a JSON `POST` for every event |
| **MQTT** | *Integrations > MQTT*: topics `<topic>/events`, `<topic>/events/<type>` and a retained `<topic>/state` (online/offline) |
| **FTP** | Snapshots uploaded to an FTP server |
| **Relay** | A GPIO pin pulses when a rule fires (open a gate, switch a lamp). Pin numbers depend on your board |

Every target has its own queue stored on the camera: if the network is down, events wait and are retried for up to 24 hours, even
across a reboot.

**Webhook example.**

```json
{"event": {"id": "20261021-190555-8f10", "ts": 1792589755683, "time": "2026-10-21T19:05:55",
           "type": "track", "rule": "Vehicle log", "label": "car", "track_id": 12, "score": 0.81,
           "box": [0.31, 0.42, 0.58, 0.77], "attributes": {"plate_text": "MH00PC0000"},
           "images": {"snapshot": "...jpg", "plate": "...jpg"}},
 "device": {"id": "picocam-743224", "name": "Gate", "location": "", "ip": "192.168.1.64"}}
```

The request carries `X-PicoCam-Event-Id` (use it to ignore duplicates after a retry). With a secret set, `X-PicoCam-Signature:
sha256=HMAC(secret, timestamp + "." + body)` lets you verify that it really came from your camera.

## The REST API

Create an **API key** under *Integrations > API & keys* and send it as `Authorization: Bearer pk_...` (or use normal account login).

| Call | Returns |
|---|---|
| `GET /api/status` | health, versions, board details |
| `GET /api/snapshot.jpg` | a picture now |
| `GET /api/events?type=&label=&q=&date=&limit=` | events |
| `GET /api/events/export.csv` | events as CSV |
| `GET /api/events/<id>/<image>.jpg` | an event picture |
| `GET|PUT /api/config/<section>` | read or change settings (`video`, `image`, `ai`, `rules`, ...) |
| `GET /api/recordings?date=YYYY-MM-DD` | recordings of a day (files support range requests) |

A short script:

```sh
curl -H "Authorization: Bearer pk_yourkey" http://<ip>/api/status
curl -H "Authorization: Bearer pk_yourkey" -o now.jpg http://<ip>/snapshot.jpg
```

## Many cameras in one place: the EdgeAI server

`server/` in this repository is a small optional program (Go) that receives events from several cameras and shows them in one
dashboard. Point each camera's *Integrations > EdgeAI server* at it. It is not needed for normal use.

## Ports used

| Port | Use | Change in |
|---|---|---|
| 80 / 443 | web (HTTP / HTTPS) | Security |
| 554 | RTSP | Integrations |
| 3702 (UDP) | ONVIF discovery | switch off in Integrations |
| 22 | SSH | Security |

If a port is already taken PicoCam falls back to 80, then 8080 for the web page, so the portal is never lost.
