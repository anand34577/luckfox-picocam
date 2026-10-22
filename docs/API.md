# EdgeAI Server API

The EdgeAI server (`server/`) collects what your cameras detect and exposes it over a REST API, so your own application can show
faces, people, vehicles and number plates, with their pictures.

- **Interactive reference:** `http://<server>:8090/api/v1/docs` (needs internet for the page's Swagger UI)
- **Machine-readable spec (OpenAPI 3):** `http://<server>:8090/api/v1/openapi.json`, import it into Postman, Insomnia, or a code generator
- Examples below use `$BASE` = `http://192.168.1.10:8090` and `$KEY` = your API key.

## Contents

1. [Concepts](#concepts)
2. [Authentication and setup](#authentication-and-setup)
3. [What you get per kind of detection](#what-you-get-per-kind-of-detection)
4. [Endpoints](#endpoints)
5. [Getting new detections as they happen](#getting-new-detections-as-they-happen) (webhooks, WebSocket, polling)
6. [Errors, paging, time formats](#errors-paging-time-formats)
7. [Alerts, health monitoring and automation](#alerts-health-monitoring-and-automation)
8. [Reports, timelines and plate clean-up](#reports-timelines-and-plate-clean-up)
9. [Retention, backup, audit and erasing data](#retention-backup-audit-and-erasing-data)
10. [Code examples](#code-examples)
11. [Privacy and security notes](#privacy-and-security-notes)

## Concepts

| Term | Meaning |
|---|---|
| **Detection** | One sighting of a face or a vehicle by a camera. Has a `type` (`face` or `vehicle`), a time, the camera, and its images. |
| **Person** | A unique face. The server groups detections of the same face under one stable person `id`. A person has no name until you give one (`PATCH /persons/{id}`). |
| **Vehicle** | A unique number plate (e.g. `MH12AB1234`) with all its sightings. You can attach an owner, a label and an allow/block *watchlist* flag to it. |
| **Device** | A camera board. Its `id` is the `device_id` on every detection. |

Face recognition here means *"same face as before"* (a stable id), not identity. A person gets a real name only when someone sets it.

## Authentication and setup

Start the server with an API key (recommended whenever it is reachable by anything other than you):

```powershell
.\edgeai-server.exe -addr :8090 -api-key "a-long-random-string" -public-url http://192.168.1.10:8090
# or: $env:EDGEAI_API_KEY = "..."
```

| Flag | Purpose |
|---|---|
| `-api-key` / `EDGEAI_API_KEY` | Require this key on the API **and on `/images/`**. Cameras posting events, `/api/v1/health` and the docs stay open. Without it, everything is open (a warning is logged). |
| `-public-url` / `EDGEAI_PUBLIC_URL` | Base of the image links in responses. Set it when the server sits behind a proxy or has a public name. Default: taken from the request's `Host` header. |
| `-cors-origin` | Let a browser app on another origin call the API (`https://myapp.example.com`, or `*`). Off by default. |

Send the key with every request, any one of:

```
Authorization: Bearer <key>
X-API-Key: <key>
?api_key=<key>        (for <img> tags and WebSockets, which cannot set headers; it ends up in logs, so prefer a header on servers)
```

A missing or wrong key gives `401` and `{"error":{"code":"unauthorized",...}}`.

**Several keys, and roles.** `-api-key` is the *master* key (admin). With it you can create more keys, one per application, and revoke any of them without touching the others:

```bash
curl -X POST -H "X-API-Key: $MASTER" -H "Content-Type: application/json" \
  -d '{"name":"reporting-app","role":"read","expires_in_days":90}' "$BASE/api/v1/keys"
```
`role` is `read` (GET only, and no access to keys, webhooks, rules, channels, settings, backup or audit) or `admin`. The secret (`ek_...`) is shown once; only a hash is stored. `GET /keys` lists names, prefixes and last use; `DELETE /keys/{id}` revokes.

**Image links are signed.** With a key required, every image URL in a response ends in `?exp=...&sig=...` and works without the key until it expires (`-image-url-ttl`, default 24 h), so an `<img>` tag or a webhook consumer can show it directly. Fetch fresh data to get fresh links. Plain `/images/...` paths without a signature need the key.

**Dashboard.** The bundled dashboard shows a sign-in form when a key is required: enter any key. That sets an HttpOnly, SameSite=Strict session cookie for 12 hours (`POST /login`, `POST /logout`, `GET /session`). After 8 wrong keys from one address, logins are refused for 5 minutes. A `read` key can look but not change anything.

## What you get per kind of detection

Every detection has the same envelope, plus a typed block:

### Person (face detection)

| You need | Field |
|---|---|
| Full image (whole frame) | `images.full` |
| Face (cropped) | `images.face` |
| Person's name, if saved | `person.name` (empty string until named) and `person.named` (`true`/`false`) |
| Stable person id | `person.id` |
| Face quality (0 to 1) | `quality` |
| Which camera / when | `device_id`, `time` (RFC 3339), `ts_ms` |

### Vehicle (with number plate)

| You need | Field |
|---|---|
| Full image (whole frame) | `images.full` |
| Number plate (cropped) | `images.plate` |
| Vehicle (cropped) | `images.vehicle` |
| Vehicle number | `vehicle.plate.text` (`null` plate object if none was read) |
| Plate has a valid format / confidence | `vehicle.plate.valid`, `vehicle.plate.confidence` |
| Vehicle type | `vehicle.type` (`car`, `truck`, `bus`, `motorcycle`, ...) and `vehicle.confidence` |
| Owner, label, allow/block flag | `vehicle.owner`, `vehicle.label`, `vehicle.watchlist` (`""`, `allow`, `block`) |

All image fields are **absolute URLs**; a field is left out when that image does not exist. Fetch them with the same API key as any
other request.

### Example: a face detection

```json
{
  "id": 4812,
  "type": "face",
  "ts_ms": 1792660123456,
  "time": "2026-10-22T09:15:23Z",
  "device_id": "gate-cam",
  "track_id": 17,
  "images": {
    "full": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660123456_17_frame.jpg",
    "face": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660123456_17_face.jpg"
  },
  "person": { "id": 31, "name": "Alice Johnson", "named": true },
  "quality": 0.74,
  "meta": { "det_conf": 0.93, "face_box": [412, 128, 498, 236], "frame_w": 1920, "frame_h": 1080 }
}
```

### Example: a vehicle detection

```json
{
  "id": 4813,
  "type": "vehicle",
  "ts_ms": 1792660130001,
  "time": "2026-10-22T09:15:30Z",
  "device_id": "gate-cam",
  "track_id": 18,
  "images": {
    "full": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660130001_18_frame.jpg",
    "plate": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660130001_18_plate.jpg",
    "vehicle": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660130001_18_vehicle.jpg"
  },
  "vehicle": {
    "type": "car",
    "confidence": 0.91,
    "plate": { "text": "MH12AB1234", "valid": true, "confidence": 0.88 },
    "owner": "Alice Johnson",
    "label": "Family car",
    "watchlist": "allow"
  },
  "meta": { "vehicle_box": [220, 340, 760, 720], "plate_box": [410, 610, 560, 660], "dwell_ms": 2400 }
}
```

`meta` is the camera's raw metadata (boxes in frame pixels, landmarks, dwell time, ...). Its keys can grow; do not depend on
keys you do not need.

## Endpoints

All paths start with `/api/v1`. Bodies are JSON. Lists use the [paging envelope](#errors-paging-time-formats).

### Detections

#### `GET /detections`
Newest first. Filters (all optional, combine freely):

| Param | Meaning |
|---|---|
| `type` | `face` or `vehicle` |
| `device` | camera id |
| `person_id` | only this person |
| `named` | `true`: faces of named people; `false`: faces of unnamed people |
| `plate` | plate text; case, spaces and dashes ignored; substring match unless `exact=true` |
| `plate_valid` | `true`/`false` |
| `vehicle_type` | e.g. `car` |
| `from`, `to` | time range, `from` inclusive, `to` exclusive ([formats](#errors-paging-time-formats)) |
| `min_quality` | minimum face quality, 0 to 1 |
| `cursor`, `limit` | paging; `limit` default 50, max 500 |

```bash
curl -H "Authorization: Bearer $KEY" \
  "$BASE/api/v1/detections?type=vehicle&plate=MH12AB1234&from=2026-10-22T00:00:00Z&limit=20"
```
```json
{ "data": [ { "...": "detection objects as above" } ], "total": 2, "next_cursor": null }
```

#### `GET /detections/{id}`
One detection (same shape). `404` if it does not exist.

#### `DELETE /detections/{id}`
Removes the detection and its image files (an image still used as a person's cover is kept). `{"ok":true}`.

#### `GET /export/detections.csv`
Same filters (no `cursor`); `limit` default 10000, max 100000. Columns: `id,type,time,ts_ms,device_id,person_id,person_name,vehicle_type,plate,plate_valid,plate_confidence,quality,full_image,face_image,plate_image,vehicle_image`.

### People

#### `GET /people`
Persons, most recently seen first (max 500). `q` matches the name or exact id; `named=true|false` filters.

```json
{
  "data": [{
    "id": 31, "name": "Alice Johnson", "named": true,
    "first_seen": 1792500000000, "first_seen_time": "2026-10-20T13:20:00Z",
    "last_seen": 1792660123456, "last_seen_time": "2026-10-22T09:15:23Z",
    "visits": 12,
    "cover_url": "/images/2026-10-22/gate-cam_1792660123456_17_face.jpg",
    "images": { "face": "http://192.168.1.10:8090/images/2026-10-22/gate-cam_1792660123456_17_face.jpg" }
  }],
  "total": 1, "next_cursor": null
}
```
`images.face` is the best face crop the server has for this person. `cover_url` is the same picture as a relative path (kept for the bundled dashboard).
A *visit* is one camera track: walking through the picture once counts once.

#### `GET /people/{id}?recent=10`
The person above plus `detections` (total count), `devices` (per-camera counts) and `recent` (their latest detections, each with
`images.full` and `images.face`). This is the one call for a person's profile page.

#### `PATCH /persons/{id}`: name a person
```bash
curl -X PATCH -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"name":"Alice Johnson"}' "$BASE/api/v1/persons/31"
```
`{"ok":true}`. Send `"name":""` to clear it. Every earlier and later detection of this person now carries the name.

#### `POST /persons/{id}/merge/{from}`
If one real person got two ids, merge `from` into `id`; detections and face samples move over. `DELETE /persons/{id}` removes a person (their detections stay, without a person).

### Vehicles

A vehicle is a plate that was read at least once. Vehicles whose plate was unreadable show up only in `/detections`.

#### `GET /vehicles`
Most recently seen first. Filters: `plate` (substring), `vehicle_type`, `plate_valid=true`, `watchlist=allow|block`, `from`, `to`, `cursor`, `limit`.

```json
{
  "data": [{
    "plate": "MH12AB1234", "plate_valid": true, "vehicle_types": ["car"],
    "sightings": 7,
    "first_seen": 1792500000000, "first_seen_time": "2026-10-20T13:20:00Z",
    "last_seen": 1792660130001,  "last_seen_time": "2026-10-22T09:15:30Z",
    "owner": "Alice Johnson", "label": "Family car", "watchlist": "allow", "notes": "",
    "images": {
      "full": "http://192.168.1.10:8090/images/...frame.jpg",
      "plate": "http://192.168.1.10:8090/images/...plate.jpg",
      "vehicle": "http://192.168.1.10:8090/images/...vehicle.jpg"
    },
    "latest_detection_id": 4813
  }],
  "total": 1, "next_cursor": null
}
```
`images` come from the latest sighting. `total` counts distinct plates matching the filters.

#### `GET /vehicles/{plate}?recent=10`
The vehicle plus `devices` (per-camera counts) and `recent` sightings (full detections with all images). The plate in the path ignores case, spaces and dashes. `404` if never seen.

#### `PATCH /vehicles/{plate}`: owner, label, watchlist
```bash
curl -X PATCH -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"owner":"Alice Johnson","label":"Family car","watchlist":"allow"}' "$BASE/api/v1/vehicles/MH12AB1234"
```
Fields: `owner`, `label`, `notes` (text), `watchlist` (`""`, `"allow"`, `"block"`). Omitted fields stay as they are. You can label a plate *before* it is ever seen, so a `block`ed plate is flagged from its first sighting. Returns the stored record. `DELETE /vehicles/{plate}` removes the record (sightings stay).

The watchlist is only a flag. The server does not open gates or raise alarms; your application (via a [webhook](#webhooks)) decides what to do when `vehicle.watchlist` is `"block"`.

### Devices and system

| Endpoint | Returns |
|---|---|
| `GET /health` | `{"status":"ok","version","time_ms","detections","devices_online","auth_required"}`. Open, no key needed; use it for monitoring. |
| `GET /devices`, `GET /devices/{id}` | Cameras: `id`, `profile` (`anpr` or `face`), `ip`, `online`, `last_seen`, and the latest heartbeat in `status` (fps, memory, stream paths). `online` means a heartbeat in the last 35 s. |
| `GET /stats` | Counters since local midnight: `vehicles_today`, `plates_today`, `faces_today`, `persons_total`, `new_persons_today`, `by_vehicle_type`. |

## Getting new detections as they happen

### Webhooks
The server POSTs each new detection to your URL. This is the most reliable way to feed your own system.

```bash
curl -X POST -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"url":"https://myapp.example.com/hooks/edgeai","types":["vehicle"]}' "$BASE/api/v1/webhooks"
```
```json
{ "id": 1, "url": "https://myapp.example.com/hooks/edgeai", "secret": "9f2c…", "types": ["vehicle"], "enabled": true, "created_ms": 1792660000000 }
```
**Save the `secret` now; it is never shown again.** (`types` omitted or `[]` = faces and vehicles. You may also pass your own `secret`.)

Each delivery is a `POST` with `Content-Type: application/json`:

```
X-EdgeAI-Event: detection.created
X-EdgeAI-Signature: sha256=<hex>
```
```json
{ "event": "detection.created", "delivery_id": "a1b2c3d4e5f60718", "created_ms": 1792660130050, "data": { "...": "a full detection object" } }
```

- **Verify the signature**: `hex(HMAC-SHA256(secret, raw request body))` must equal the value after `sha256=`. Compare in constant time, and compute it over the raw bytes before parsing.
- Answer with any `2xx` within 5 seconds. Otherwise the server retries after 1 s, 5 s and 25 s and then gives up (the detection stays in `/detections`; catch up with `?cursor=` or `from=`). Redirects are not followed.
- Delivery can repeat, so de-duplicate on `data.id`. Order is not guaranteed.
- Face detections can be sent more than once per person walk-by when a better picture appears (`meta.update` is `true`).

Other calls: `GET /webhooks` (list, no secrets), `DELETE /webhooks/{id}`, `POST /webhooks/{id}/test` (sends a signed `webhook.test` event and tells you what your server answered).

### WebSocket (for dashboards)
`ws://<server>:8090/api/v1/ws?type=vehicle&api_key=<key>`. `type` is optional. Messages: `{"kind":"event","data":{...}}` per detection and `{"kind":"device","data":{...}}` per camera heartbeat. The event `data` here is the compact row used by the bundled dashboard, with **relative** image paths (`frame_url`, `crop_url`, `extra_url`) and person/plate fields; for the full shape call `GET /detections/{data.id}`. There is no replay: after a reconnect, fetch what you missed with `GET /detections?cursor=` or `from=`.

### Polling
Keep the highest `id` you have seen; ask for newer ones with `from=<last time>` and de-duplicate on `id`, or page back with `cursor` until you reach a known id. Polling every few seconds is fine.

## Errors, paging, time formats

**Errors** use one shape, with a stable `code` to branch on:
```json
{ "error": { "code": "bad_request", "message": "type must be face or vehicle" } }
```
| HTTP | `code` | When |
|---|---|---|
| 400 | `bad_request` | invalid filter, id or body |
| 401 | `unauthorized` | missing or wrong API key |
| 404 | `not_found` | unknown detection, person, plate, device or webhook |
| 500 / 503 | `internal` / `unavailable` | server or database problem |

(The older dashboard routes `GET /events`, `GET /persons`, ... keep their original plain-text errors and unwrapped list responses. New code should use the endpoints in this document.)

**Paging.** Lists return `{"data":[...], "total": N, "next_cursor": 4800 | null}`. `total` is the number of matches overall. While `next_cursor` is not `null`, request the next page with `?cursor=<next_cursor>` and the same filters. Do not build cursors yourself.

**Time.** Responses give both `ts_ms` (epoch milliseconds, UTC) and `time` (RFC 3339, UTC). Request parameters `from` and `to` accept epoch seconds, epoch milliseconds (values below 10^11 are read as seconds), or RFC 3339 such as `2026-10-22T09:00:00+05:30`.

## Alerts, health monitoring and automation

Alerts are *rules* that send a message through one or more *channels*. All of this needs an admin key. Nothing runs on the cameras: the server checks each stored detection against the rules in memory (no extra database reads), and at most 4 messages are in flight at once.

### 1. Add a channel

| `kind` | `config` | Notes |
|---|---|---|
| `telegram` | `bot_token`, `chat_id` | Sends the full frame as a photo with the text. |
| `ntfy` | `url` (e.g. `https://ntfy.sh/my-topic`), optional `token` | Push notification to phones; the frame is attached. |
| `email` | `host`, `from`, `to` (comma list), optional `port`, `security` (`starttls` default, `tls`, `none`), `username`, `password` | The frame is attached. The password is only sent over an encrypted link. |
| `http` | `url`, optional `secret` | POSTs `{event:"alert", data:{rule, message, detection}}`, signed like a webhook. Use this for WhatsApp/SMS gateways, Home Assistant, Slack, your own app. |

```bash
curl -X POST -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"name":"Security phone","kind":"ntfy","config":{"url":"https://ntfy.sh/my-secret-topic"}}' "$BASE/api/v1/channels"
curl -X POST -H "Authorization: Bearer $KEY" "$BASE/api/v1/channels/1/test"     # {"ok":true}
```
Secrets (`bot_token`, `password`, `token`, `secret`) come back as `********`; send that value back unchanged when editing to keep the stored one. A channel that a rule uses cannot be deleted.

### 2. Add a rule

```json
{
  "name": "Blocked plate at the gate",
  "on": "detection",
  "channels": [1],
  "cooldown_s": 300,
  "match": { "type": "vehicle", "watchlist": "block", "device": "gate-cam" }
}
```

`on` decides what the rule watches, and `match` narrows it:

| `on` | `match` fields |
|---|---|
| `detection` | `type` (`face`/`vehicle`), `device`; vehicles: `watchlist` (`allow`/`block`), `plate_pattern` (`MH12*`, `*` and `?`), `plate_read` (`false` = unreadable plates); faces: `face` (`unknown`/`named`), `min_quality` |
| `device_offline`, `device_online` | `device`. Offline fires once, 60 s after the camera is last heard from; online fires when it returns. |
| `device_health` | `metric` (`temp_c`, `cpu_pct`, `npu_load_pct`, `mem_avail_kb`, `cma_pct`), `op` (`>`/`<`), `value`, optional `device` |
| `server_disk` | `op`, `value` = percent of free disk on the server's data drive, e.g. `"op":"<","value":10` |

Every rule can also have `hours` (`"22:00-06:00"`, may cross midnight), `days` (0 = Sunday), and `tz` (IANA name; default is the server's zone). `cooldown_s` is the minimum gap per *subject*: the same plate, the same person, or the same camera, so one unknown visitor produces one alert, not one per frame. Invalid rules are rejected with the reason.

Examples:
- Unknown face at night: `{"on":"detection","match":{"type":"face","face":"unknown","hours":"22:00-06:00","tz":"Asia/Kolkata"}}`
- Camera too hot: `{"on":"device_health","cooldown_s":900,"match":{"metric":"temp_c","op":">","value":80}}`
- Disk nearly full: `{"on":"server_disk","cooldown_s":3600,"match":{"op":"<","value":10}}`

`POST /rules/{id}/test` with `{"detection_id": 123}` tells you whether the rule would fire for that past detection, and why not, without sending anything. `GET /alerts` is the history with delivery results (`delivered`, `failed`, `error`). Rules are replaced as a whole with `PUT /rules/{id}`.

Health rules rely on the camera heartbeat. The temperature, CPU and NPU load fields need PicoCam firmware that sends them (the firmware in this repository does, once rebuilt and deployed); older firmware still works for offline/online, memory and media-memory rules.

**Gates and relays.** The camera already switches its own GPIO relay when it sees a plate (set in the PicoCam web UI), without waiting for the network. The server does not duplicate that. Use alerts to tell people, and the camera's relay for the gate.

## Reports, timelines and plate clean-up

All times follow `tz`, with exact day boundaries across daylight-saving changes. A range may span at most 93 days.

| Endpoint | What it gives |
|---|---|
| `GET /reports/summary?from=2026-10-20&tz=Asia/Kolkata` | `totals` (detections, faces, vehicles, plates read, unique, new and unnamed people, unique plates), `by_period` (per hour for up to 2 days, else per day; override with `group=hour` or `group=day`), `by_vehicle_type`, `by_device`, `top_plates` with owner and label. |
| `GET /reports/attendance?kind=person&from=...&to=...&named=true&format=csv` | One row per subject per day: `first_seen`, `last_seen`, `duration_min`, `sightings`, `devices`. Attendance for named people (`kind=person`), first in / last out for plates (`kind=vehicle`). |
| `GET /people/{id}/timeline`, `GET /vehicles/{plate}/timeline` | Where a person or vehicle went: `visits` (consecutive sightings on one camera, within `gap` seconds, default 120, are one visit, with the first sighting's pictures), `path` (cameras in order) and `transitions` (with the gap between cameras). Up to 5000 sightings; `truncated` says if more existed. |
| `GET /vehicles/{plate}/similar?distance=1` | Plates one character away from this one. Usually OCR misreads (`B` read as `8`). |
| `POST /vehicles/{plate}/merge/{from}` | Moves all sightings of the misread plate `from` onto `plate`; keeps `plate`'s label, or takes over `from`'s if it has none. |

The timeline follows the *same face id or plate* across cameras. A face the server could not match to the same person will appear as two people; merge them with `POST /persons/{id}/merge/{from}`.

## Retention, backup, audit and erasing data

**Automatic cleanup** runs one minute after start and then hourly, in small batches so camera uploads are not held up:

```bash
curl -X PUT -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"days":30,"protected_days":365,"max_disk_gb":50}' "$BASE/api/v1/settings/retention"
```
- `days`: delete detections and their pictures older than this. `0` = keep forever.
- `protected_days`: the same for *protected* detections: faces of **named people** and plates that have an **owner, label or watchlist** entry. `0` = keep forever.
- `max_disk_gb`: cap for the pictures folder. When over, the oldest *unprotected* detections go first until it is at 90%. If only protected ones remain, nothing more is deleted and the result says so.
- Unnamed people left with no detections are removed too.

`GET /maintenance` shows disk use, free space and the last result; `POST /maintenance/run` runs it now. Pair it with a `server_disk` alert rule.

**Backup:** `GET /backup` downloads a zip with a consistent copy of the database; add `?images=true` for the pictures. It is not compressed (fast, low CPU). The database copy briefly pauses writes, so run it off-peak on very large databases. The zip also contains the signing secret and hashed keys: store it like a password. To restore, stop the server, unzip into the `-data` folder, start the server.

**Audit log:** `GET /audit` lists every change made through the API (who, from where, what, result). Reads and camera uploads are not logged.

**Erasing data (privacy requests):**
- `POST /people/{id}/erase`: deletes the person, every detection and picture of them, and their face samples.
- `POST /vehicles/{plate}/erase`: deletes every sighting, picture and the label of a plate.
- `DELETE /persons/{id}` (older route) only deletes the person record and keeps the detections.

Erased data is gone from the server. Copies in backups, webhook receivers, or alert messages (Telegram, email) are not touched.

## Code examples

### JavaScript / TypeScript (Node 18+ or browser)
```ts
const BASE = "http://192.168.1.10:8090";
const headers = { Authorization: `Bearer ${process.env.EDGEAI_KEY}` };

async function* detections(params: Record<string, string> = {}) {
  let cursor: number | null = null;
  do {
    const q = new URLSearchParams({ ...params, limit: "200", ...(cursor ? { cursor: String(cursor) } : {}) });
    const r = await fetch(`${BASE}/api/v1/detections?${q}`, { headers });
    if (!r.ok) throw new Error((await r.json()).error.message);
    const page = await r.json();
    yield* page.data;
    cursor = page.next_cursor;
  } while (cursor);
}

for await (const d of detections({ type: "vehicle", plate: "MH12AB1234" })) {
  console.log(d.time, d.vehicle.plate.text, d.images.plate);
}

// Show a protected image in a page: fetch it with the header, then use a blob URL
const blob = await (await fetch(url, { headers })).blob();
img.src = URL.createObjectURL(blob);
```

### Python
```python
import os, requests

BASE = "http://192.168.1.10:8090"
S = requests.Session()
S.headers["Authorization"] = f"Bearer {os.environ['EDGEAI_KEY']}"

# a person's profile: name, face, and the last full frames
p = S.get(f"{BASE}/api/v1/people/31").json()
print(p["name"] or "(unnamed)", p["images"]["face"])
for d in p["recent"]:
    print(d["time"], d["device_id"], d["images"]["full"])

# download the cropped plate of every sighting of one vehicle
v = S.get(f"{BASE}/api/v1/vehicles/MH12AB1234", params={"recent": 100}).json()
for d in v["recent"]:
    open(f"plate_{d['id']}.jpg", "wb").write(S.get(d["images"]["plate"]).content)

# name someone, flag a plate
S.patch(f"{BASE}/api/v1/persons/31", json={"name": "Alice Johnson"})
S.patch(f"{BASE}/api/v1/vehicles/KA01XY9999", json={"watchlist": "block", "label": "Unknown van"})
```

### Receiving a webhook (Python / Flask)
```python
import hmac, hashlib
from flask import Flask, request, abort

SECRET = b"the-secret-from-creation"
app = Flask(__name__)

@app.post("/hooks/edgeai")
def hook():
    raw = request.get_data()
    expected = "sha256=" + hmac.new(SECRET, raw, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, request.headers.get("X-EdgeAI-Signature", "")):
        abort(401)
    msg = request.get_json()
    if msg["event"] == "detection.created":
        d = msg["data"]
        if d["type"] == "vehicle" and d["vehicle"]["watchlist"] == "block":
            alert(d["vehicle"]["plate"]["text"], d["images"]["full"])
        elif d["type"] == "face":
            who = d["person"]["name"] or f"person #{d['person']['id']}"
            log_visit(who, d["images"]["face"], d["images"]["full"])
    return "", 204
```

### Receiving a webhook (Node / Express)
```js
import express from "express";
import crypto from "node:crypto";

const app = express();
app.post("/hooks/edgeai", express.raw({ type: "application/json" }), (req, res) => {
  const sig = "sha256=" + crypto.createHmac("sha256", process.env.EDGEAI_SECRET).update(req.body).digest("hex");
  const got = req.get("X-EdgeAI-Signature") || "";
  if (got.length !== sig.length || !crypto.timingSafeEqual(Buffer.from(got), Buffer.from(sig))) return res.sendStatus(401);
  const { event, data } = JSON.parse(req.body);
  if (event === "detection.created") console.log(data.type, data.id, data.images.full);
  res.sendStatus(204);
});
app.listen(3000);
```

## Privacy and security notes

- Faces and number plates are personal data. Use `-api-key`, put the server behind HTTPS (a reverse proxy; then set `-public-url https://...`), and keep it off the public internet unless you need it there.
- With `-api-key`, image links are signed and expire; whoever receives a link can use it until then. Without `-api-key`, anyone who can reach the port can fetch any image.
- Webhook URLs are called from the server's network position. Only admin keys can add one, so keep those private.
- Retention: set `days` / `max_disk_gb` (see above); by default nothing is deleted automatically. A detection with its pictures takes a few hundred KB.
- `http://` webhooks and API calls send the key and the pictures in clear text. Use HTTPS outside a trusted LAN.
