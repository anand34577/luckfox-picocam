// A pretend camera for trying the web UI without a board, and for the screenshots in the docs.
//   npm run build && npm run mock          then open http://localhost:8099/   (login: any user, any password)
// It answers the REST API with made-up data and publishes demo events over the WebSocket. Nothing here is real footage.
import { createServer } from 'node:http';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { cropSvg, sceneSvg, SCENE_BOXES } from './scene.mjs';

const here = fileURLToPath(new URL('.', import.meta.url));
const dist = join(here, '..', 'dist');
const port = Number(process.env.PORT || 8099);

// ---- configuration: the firmware's own defaults, so the forms show real default values
const cfgSrc = readFileSync(join(here, '..', '..', 'firmware', 'src', 'core', 'config.cc'), 'utf8');
const defaults = JSON.parse(cfgSrc.match(/R"JSON\(([\s\S]*?)\)JSON"/)[1]);
const config = structuredClone(defaults);
config.device = { name: 'Front gate', location: 'Main entrance', id: 'picocam-a1b2c3' };
config.video.main.width = 2304; config.video.main.height = 1296;
config.recording = { ...(config.recording || {}), mode: 'events', pre_s: 5, post_s: 10 };
config.rules = [
  { id: 'r1', name: 'Vehicle log', type: 'track', enabled: true, labels: ['car', 'truck', 'motorcycle', 'bus'], zone: [[0.05, 0.55], [0.95, 0.55], [0.95, 0.95], [0.05, 0.95]], cooldown_s: 10 },
  { id: 'r2', name: 'Pedestrians at the gate', type: 'intrusion', enabled: true, labels: ['person'], zone: [[0.78, 0.4], [0.94, 0.4], [0.94, 0.82], [0.78, 0.82]], cooldown_s: 30 },
];
config.masks = [];
config.ai = { ...config.ai, enabled: true, max_fps: 10, detector: { model: 'yolov5n_coco_320.rknn', conf: 0.4, nms: 0.45, classes: ['person', 'car', 'truck', 'bus', 'motorcycle'], min_box_px: 12 },
  stages: [{ id: 's1', model: 'plate_yolo11n_320.rknn', on: ['car', 'truck', 'bus'], kind: 'detect' }, { id: 's2', model: 'platenet_in.rknn', on: ['s1'], kind: 'ocr' }] };

// ---- demo state
const day = '2026-10-21';
const now = () => Date.now();
const mkEvent = (i, label, rule, plate, mins) => {
  const ts = new Date(`${day}T18:40:00`).getTime() - mins * 60000;
  const t = new Date(ts - new Date(ts).getTimezoneOffset() * 60000).toISOString().slice(0, 19);
  const id = `20261021-${t.slice(11, 19).replace(/:/g, '')}-d${i}`;
  return {
    id, ts, time: t, day, type: label === 'person' ? 'intrusion' : 'track', rule, rule_id: label === 'person' ? 'r2' : 'r1', label, track_id: 40 + i,
    score: 0.78 + (i % 5) * 0.03, box: label === 'person' ? SCENE_BOXES.person : SCENE_BOXES.car,
    attributes: plate ? { plate_text: plate } : {}, images: plate ? { snapshot: 'a.jpg', object: 'o.jpg', plate: 'p.jpg' } : { snapshot: 'a.jpg', object: 'o.jpg' },
  };
};
const events = [
  mkEvent(1, 'car', 'Vehicle log', 'MH00PC0000', 0), mkEvent(2, 'person', 'Pedestrians at the gate', '', 3),
  mkEvent(3, 'car', 'Vehicle log', 'MH00PC0007', 9), mkEvent(4, 'truck', 'Vehicle log', 'MH00PC0113', 17),
  mkEvent(5, 'person', 'Pedestrians at the gate', '', 26), mkEvent(6, 'car', 'Vehicle log', 'MH00PC0042', 41),
  mkEvent(7, 'motorcycle', 'Vehicle log', 'MH00PC0555', 58), mkEvent(8, 'car', 'Vehicle log', 'MH00PC0301', 77),
  mkEvent(9, 'person', 'Pedestrians at the gate', '', 95), mkEvent(10, 'car', 'Vehicle log', 'MH00PC0911', 130),
  mkEvent(11, 'bus', 'Vehicle log', 'MH00PC0204', 170), mkEvent(12, 'car', 'Vehicle log', 'MH00PC0620', 205),
];
const hours = [0, 0, 0, 0, 1, 2, 5, 9, 14, 11, 8, 7, 9, 10, 8, 6, 9, 13, 12, 7, 4, 2, 1, 0];
const models = [
  ['yolov5n_coco_320.rknn', 'Objects (COCO 80, YOLOv5n 320)', 'detect', 2.6], ['plate_yolo11n_320.rknn', 'Number plate detector (YOLO11n 320)', 'detect', 3.7],
  ['platenet_in.rknn', 'Indian plate reader (PlateNet OCR)', 'ocr', 0.7], ['retinaface_320.rknn', 'Face detector (RetinaFace 320)', 'face_detect', 1.7],
  ['mobilefacenet.rknn', 'Face embedding (MobileFaceNet)', 'face_embed', 3.5],
].map(([id, name, task, mb]) => ({ id, name, task, source: 'bundled', size: Math.round(mb * 1048576), meta: { name, task, labels: task === 'detect' && id.startsWith('yolo') ? ['person', 'car', 'truck', 'bus', 'motorcycle', 'bicycle'] : ['plate'] }, inputs: [{ dims: '1x320x320x3', type: 'UINT8' }], outputs: [] }));

const board = { model: 'Luckfox Pico Plus', soc: 'RV1103', ram_mb: 128, boot: 'spi-nand', flash_limited: true, ethernet: true, wifi: false, sensor: 'sc3336', sensor_width: 2304, sensor_height: 1296, sensor_fps: 30 };
const status = () => ({
  device: { name: config.device.name, id: config.device.id, location: config.device.location, model: board.model, soc: 'Rockchip RV1103', sensor: 'sc3336 (2304x1296)', firmware: '1.2.2', kernel: '5.10.160', mac: '3a:f1:62:0c:77:10', serial: config.device.id, board },
  system: { cpu_pct: 34, load: [0.62, 0.55, 0.48], mem_total_kb: 96000, mem_avail_kb: 41000, media_mem_total_kb: 24576, media_mem_used_kb: 19800, app_rss_kb: 9400, temp_c: 52.4, npu_load_pct: 18, uptime_s: 183000, app_uptime_s: 91000, time_ms: now(), ntp_synced: true },
  video: { running: true, main: { enabled: true, codec: 'h264', width: 2304, height: 1296, fps: 25, bitrate: 4096, kbps: 3870, measured_fps: 25, clients: 1 }, sub: { enabled: true, codec: 'h264', width: 960, height: 540, fps: 25, bitrate: 1024, kbps: 940, measured_fps: 25, clients: 2 }, sensor_fps: 25 },
  isp: { running: true, mode: 'smart', scene: 'normal', exposure_ms: 8.4, gain: 2.1, env_lv: 6.2, converged: true, ev_bias: 0, drc_gain: 1, anti_flicker: false, night: false, luma: { median: 118, clip_hi_pct: 0.4, clip_lo_pct: 1.2 }, shutter_cap_ms: 20, subject: false, wb: { cct: 5600, smart: false }, histogram: [2,3,5,8,11,14,17,19,21,22,20,18,16,14,13,12,11,10,9,8,7,6,5,4,3,3,2,2,1,1,1,0] },
  rtsp_sessions: 2, rtsp_port: 554,
  ai: { enabled: true, running: true, fps: 9.6, infer_ms: 41, tracks: 3, counters: [{ name: 'Gate count', a_to_b: 128, b_to_a: 117 }] },
  motion: { enabled: true, level_pct: 1.4, events: 12 },
  storage: storage(), events: { stored: events.length, total: 212, dropped: 0, dir: '/mnt/sdcard/picocam/events' },
  network: network(), network_pending_s: 0,
});
function storage() {
  return { kind: 'sd', device: '/dev/mmcblk1p1', fs: 'vfat', path: '/mnt/sdcard/picocam', available: true, total_bytes: 63864569856, free_bytes: 41200000000, error: '', recording: true, mode: 'events', current_file: '18-31-02.mp4', current_bytes: 8300000, dropped_frames: 0, can_format: true, can_eject: true };
}
function network() {
  return { interfaces: [{ name: 'eth0', ip: '192.168.1.64', netmask: '255.255.255.0', mac: '3a:f1:62:0c:77:10', link: true, speed_mbps: 100 }, { name: 'usb0', ip: '172.32.0.93', netmask: '255.255.255.0', mac: '3a:f1:62:0c:77:11', link: false, speed_mbps: 0 }], gateway: '192.168.1.1', dns: ['192.168.1.1', '1.1.1.1'], hostname: 'picocam', primary: 'eth0' };
}
const users = [{ name: 'admin', role: 'admin' }, { name: 'gate-desk', role: 'viewer' }];
const logs = ['18:40:12 [I] ai: person #44 entered "Pedestrians at the gate"', '18:39:50 [I] events: stored 20261021-183950-d1', '18:31:02 [I] storage: new segment 18-31-02.mp4', '18:00:01 [I] system: time synced with pool.ntp.org', '17:12:44 [I] video: main h264 2304x1296@25 4096kbps | sub h264 960x540@25 1024kbps', '17:12:42 [I] main: PicoCam 1.2.2 on Luckfox Pico Plus'];

const json = (res, body, code = 200) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(JSON.stringify(body)); };
const svg = (res, s) => { res.writeHead(200, { 'content-type': 'image/svg+xml', 'cache-control': 'no-store' }); res.end(s); };
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.json': 'application/json' };
let session = { activated: true, user: 'admin', role: 'admin', device: config.device.name, model: board.model, firmware: '1.2.2', ip: '192.168.1.20' };

const server = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = url.pathname;
  let body = '';
  req.on('data', (c) => (body += c));
  req.on('end', () => {
    if (!p.startsWith('/api') && p !== '/snapshot.jpg' && p !== '/__mock/session') {
      let f = normalize(join(dist, p === '/' ? 'index.html' : p));
      if (!f.startsWith(dist) || !existsSync(f) || statSync(f).isDirectory()) f = join(dist, 'index.html');
      res.writeHead(200, { 'content-type': types[extname(f)] || 'application/octet-stream' });
      return res.end(readFileSync(f));
    }
    if (p === '/snapshot.jpg' || p === '/api/snapshot.jpg') return svg(res, sceneSvg(1280, 720));
    if (p === '/__mock/session') { try { session = { ...session, ...JSON.parse(body || '{}') }; } catch {} return json(res, session); }  // for the screenshots
    if (req.method !== 'GET') {
      if (p === '/api/login') { session = { ...session, role: 'admin' }; return json(res, session); }
      if (p === '/api/logout') { session = { ...session, role: 'none' }; return json(res, { ok: true }); }
      const m = p.match(/^\/api\/config\/(\w+)/);
      if (m && req.method === 'PUT') { try { Object.assign(config[m[1]] ??= {}, JSON.parse(body || '{}')); } catch {} return json(res, config[m[1]]); }
      return json(res, { ok: true });
    }
    if (p === '/api/session') return json(res, session);
    if (p === '/api/status') return json(res, status());
    let m = p.match(/^\/api\/config\/(\w+)/);
    if (m) return json(res, m[1] === 'defaults' ? defaults : config[m[1]] ?? {});
    if (p === '/api/events/summary') return json(res, { today: events.length, hours, by_label: { car: 7, person: 3, truck: 1, bus: 1 }, by_type: { track: 9, intrusion: 3 } });
    m = p.match(/^\/api\/events\/([^/]+)\/(.+)\.jpg$/);
    if (m) return svg(res, cropSvg(m[2] === 'p' ? 'plate' : m[2] === 'o' ? 'object' : 'scene'));
    if (p === '/api/events') return json(res, events);
    if (p === '/api/storage') return json(res, storage());
    if (p === '/api/recordings/days') return json(res, [day, '2026-10-20']);
    if (p === '/api/recordings') return json(res, [18, 17, 16].flatMap((h) => [0, 1].map((k) => ({ name: `${h}-${k ? '31' : '01'}-02.mp4`, size: 38000000 + k * 9000000, start: `${day}T${h}:${k ? '31' : '01'}:02`, duration_s: 1800 }))));
    if (p === '/api/system/update-state') return json(res, { version: '1.2.2', pending: false, rolled_back: '', config_recovered: false, can_rollback: true, previous: '1.2.0' });
    if (p === '/api/system/media-memory') return json(res, { active_mb: 24, configured_mb: 24, used_kb: 19800, total_ram_kb: 96000, changeable: false, note: 'the boot environment cannot be edited from Linux on this board (SPI-NAND boot). Change rk_dma_heap_cma in the SDK\'s env and reflash. See docs/BOARDS.md', options: [24, 28, 32, 40] });
    if (p === '/api/system/logs') return json(res, { lines: logs });
    if (p === '/api/system/audit') return json(res, []);
    if (p === '/api/system/security') return json(res, { watchdog: true, watchdog_armed: true, ssh: true, adb: false, root_sync: true, root_synced: 1, ssh_running: true });
    if (p === '/api/system/tls') return json(res, { enabled: true, self_signed: true, fingerprint: 'AB:12:CD:34:EF:56', available: true });
    if (p === '/api/ai/models') return json(res, models);
    if (p === '/api/ai/plugins') return json(res, []);
    if (p === '/api/users') return json(res, users);
    if (p === '/api/sessions') return json(res, []);
    if (p === '/api/keys') return json(res, []);
    if (p === '/api/faces') return json(res, []);
    if (p === '/api/integrations/status') return json(res, { targets: [], outbox: 0 });
    return json(res, {});
  });
});

// realtime: status every 2 s, a detection set that matches the generated scene
const wss = new WebSocketServer({ noServer: true });
const live = new WebSocketServer({ noServer: true });
server.on('upgrade', (req, sock, head) => {
  const p = new URL(req.url, 'http://x').pathname;
  const target = p === '/ws/live' ? live : wss;
  target.handleUpgrade(req, sock, head, (ws) => target.emit('connection', ws, req));
});
wss.on('connection', (ws) => {
  const send = (o) => ws.readyState === 1 && ws.send(JSON.stringify(o));
  send({ topic: 'status', ...status() });
  const t1 = setInterval(() => send({ topic: 'status', ...status() }), 2000);
  const t2 = setInterval(() => {
    send({ topic: 'detections', pts_us: 0, objects: [
      { id: 41, label: 'car', score: 0.91, box: SCENE_BOXES.car, part: SCENE_BOXES.plate, attrs: { plate_text: 'MH00PC0000' } },
      { id: 44, label: 'person', score: 0.87, box: SCENE_BOXES.person, attrs: {} },
      { id: 42, label: 'car', score: 0.74, box: SCENE_BOXES.car2, attrs: {} } ] });
  }, 300);
  ws.on('close', () => { clearInterval(t1); clearInterval(t2); });
});
live.on('connection', () => {});  // no video in the mock: the player stays on "connecting"
server.listen(port, () => console.log(`mock camera on http://localhost:${port}/ (serving ${dist})`));
