// Low-latency live player for /ws/live.
//  - WebCodecs (secure contexts: HTTPS or localhost): Annex-B frames decoded straight to a canvas, ~1 frame delay.
//  - Media Source Extensions (plain HTTP on the LAN): one fMP4 fragment per frame; the player keeps the playhead
//    pinned to the live edge so delay stays well under a second.
export interface PlayerStats {
  mode: string;
  codec: string;
  width: number;
  height: number;
  fps: number;
  kbps: number;
  delayMs: number;
}

type State = 'connecting' | 'playing' | 'error' | 'stopped';

export class LivePlayer {
  private ws: WebSocket | null = null;
  private decoder: any = null;
  private ms: MediaSource | null = null;
  private sb: SourceBuffer | null = null;
  private queue: ArrayBuffer[] = [];
  private frames = 0;
  private bytes = 0;
  private t0 = performance.now();
  private needKey = true;
  private retry: any = null;
  private stopped = false;
  private initInfo: any = null;
  private ptsRing = new Map<number, number>();  // frame index since init -> capture time (us)
  private frameIdx = 0;
  private dur = 40000;  // frame duration of the fMP4 timeline (us)
  private shownPts: number | null = null;
  stats: PlayerStats = { mode: '', codec: '', width: 0, height: 0, fps: 0, kbps: 0, delayMs: 0 };

  constructor(
    private canvas: HTMLCanvasElement,
    private video: HTMLVideoElement,
    private onState: (s: State, msg?: string) => void,
    private onStats?: (s: PlayerStats) => void,
  ) {}

  // capture time (us, the stream's clock) of the picture on screen, or null when unknown
  currentPts(): number | null {
    if (!this.ms) return this.shownPts;
    const k = Math.round((this.video.currentTime * 1e6) / this.dur);
    for (const d of [0, -1, 1, -2, 2, -3, 3]) {
      const v = this.ptsRing.get(k + d);
      if (v !== undefined) return v;
    }
    return null;
  }

  static webcodecs() {
    return typeof (window as any).VideoDecoder === 'function' && window.isSecureContext;
  }

  start(stream: 'main' | 'sub') {
    this.stop();
    this.stopped = false;
    const mp4 = !LivePlayer.webcodecs();
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    this.onState('connecting');
    const ws = new WebSocket(`${proto}://${location.host}/ws/live?stream=${stream}${mp4 ? '&fmt=mp4' : ''}`);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onmessage = (e) => (typeof e.data === 'string' ? this.onInit(JSON.parse(e.data)) : this.onFrame(e.data));
    ws.onclose = () => {
      // a socket replaced by start()/stop() closes later; it must neither retry nor start a second connection
      if (this.stopped || this.ws !== ws) return;
      this.onState('error', 'Connection lost, reconnecting…');
      this.retry = setTimeout(() => this.start(stream), 2000);
    };
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.retry);
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.onmessage = null;
      this.ws.close();
    }
    this.ws = null;
    try {
      this.decoder?.close();
    } catch {}
    this.decoder = null;
    if (this.ms) {
      try {
        this.video.pause();
        this.video.removeAttribute('src');
        this.video.load();
      } catch {}
    }
    this.ms = null;
    this.sb = null;
    this.queue = [];
    this.needKey = true;
    this.onState('stopped');
  }

  private onInit(m: any) {
    if (m.type !== 'init') return;
    this.initInfo = m;
    this.ptsRing.clear();
    this.frameIdx = 0;
    this.dur = 1e6 / Math.max(1, m.fps || 25);
    this.stats = { ...this.stats, codec: m.codec, width: m.width, height: m.height, mode: m.transport === 'fmp4' ? 'MSE' : 'WebCodecs' };
    this.needKey = true;
    if (m.transport === 'fmp4') this.setupMse(m);
    else this.setupDecoder(m);
  }

  private setupDecoder(m: any) {
    try {
      this.decoder?.close();
    } catch {}
    const VD = (window as any).VideoDecoder;
    const ctx = this.canvas.getContext('2d')!;
    this.canvas.width = m.width;
    this.canvas.height = m.height;
    this.decoder = new VD({
      output: (frame: any) => {
        ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
        this.shownPts = frame.timestamp;
        frame.close();
        this.frames++;
        this.onState('playing');
      },
      error: (e: any) => {
        this.onState('error', 'Decoder error: ' + e.message);
        this.needKey = true;
        this.ws?.send('{"request":"keyframe"}');
        setTimeout(() => this.initInfo && this.setupDecoder(this.initInfo), 200);
      },
    });
    this.decoder.configure({ codec: m.codec, optimizeForLatency: true, hardwareAcceleration: 'no-preference' });
  }

  private setupMse(m: any) {
    const mime = `video/mp4; codecs="${m.codec}"`;
    if (!(window as any).MediaSource || !MediaSource.isTypeSupported(mime)) {
      this.onState('error', `This browser cannot play ${m.format.toUpperCase()} (${m.codec}). Use H.264 for the stream or open the page in Chrome/Edge.`);
      return;
    }
    this.queue = [];
    const ms = new MediaSource();
    this.ms = ms;
    this.video.src = URL.createObjectURL(ms);
    this.video.muted = true;
    ms.addEventListener('sourceopen', () => {
      const sb = ms.addSourceBuffer(mime);
      sb.mode = 'segments';
      this.sb = sb;
      sb.addEventListener('updateend', () => this.pump());
      this.pump();
    });
    this.video.play().catch(() => {});
  }

  private pump() {
    const sb = this.sb;
    if (!sb || sb.updating || !this.queue.length) return;
    try {
      sb.appendBuffer(this.queue.shift()!);
    } catch (e: any) {
      if (e.name === 'QuotaExceededError') this.trim(true);
    }
    this.trim(false);
  }

  // keep only a few seconds buffered and the playhead at the live edge
  private trim(force: boolean) {
    const v = this.video, sb = this.sb;
    if (!sb || !v.buffered.length) return;
    const end = v.buffered.end(v.buffered.length - 1);
    const lag = end - v.currentTime;
    this.stats.delayMs = Math.round(lag * 1000);
    if (lag > 1.2) v.currentTime = end - 0.15;
    else if (lag > 0.45) v.playbackRate = 1.15;
    else v.playbackRate = 1;
    if (v.paused) v.play().catch(() => {});
    const start = v.buffered.start(0);
    if ((force || v.currentTime - start > 10) && !sb.updating) {
      try {
        sb.remove(start, v.currentTime - 2);
      } catch {}
    }
  }

  private onFrame(buf: ArrayBuffer) {
    const d = new DataView(buf);
    const flags = d.getUint8(0);
    const pts = Number(d.getBigUint64(1));
    const payload = buf.slice(9);
    this.bytes += buf.byteLength;
    const now = performance.now();
    if (now - this.t0 > 1000) {
      this.stats.fps = Math.round((this.frames * 1000) / (now - this.t0));
      this.stats.kbps = Math.round((this.bytes * 8) / (now - this.t0));
      this.frames = 0;
      this.bytes = 0;
      this.t0 = now;
      this.onStats?.({ ...this.stats });
    }
    if (flags & 2) {
      // fMP4 init segment
      this.queue.push(payload);
      this.pump();
      return;
    }
    const key = (flags & 1) === 1;
    // the fMP4 timeline advances one frame duration per frame the server sent, whether or not this player used it
    this.ptsRing.set(this.frameIdx++, pts);
    this.ptsRing.delete(this.frameIdx - 700);
    if (this.needKey && !key) return;
    this.needKey = false;
    if (this.ms) {
      this.queue.push(payload);
      this.frames++;
      this.onState('playing');
      // fall too far behind (background tab): drop and resync on the next keyframe
      if (this.queue.length > 60) {
        this.queue = [];
        this.needKey = true;
      }
      this.pump();
      return;
    }
    const dec = this.decoder;
    if (!dec || dec.state !== 'configured') return;
    if (dec.decodeQueueSize > 4) {
      // can't keep up: skip to the next keyframe rather than build delay
      this.needKey = true;
      this.ws?.send('{"request":"keyframe"}');
      return;
    }
    const EVC = (window as any).EncodedVideoChunk;
    dec.decode(new EVC({ type: key ? 'key' : 'delta', timestamp: pts, data: payload }));
    this.stats.delayMs = Math.round(dec.decodeQueueSize * (1000 / Math.max(1, this.initInfo?.fps || 25)));
  }
}
