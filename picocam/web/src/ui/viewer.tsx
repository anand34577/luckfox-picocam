import { useEffect, useRef, useState } from 'preact/hooks';
import { api } from '../api';
import { LivePlayer, PlayerStats } from '../player';
import { onBus } from '../store';
import { Icon } from './icons';
import { Btn, toast } from './kit';

export interface Det {
  id: number;
  label: string;
  score: number;
  box: number[];
  part?: number[];
  attrs: Record<string, string>;
}

const COLORS: Record<string, string> = { person: '#38bdf8', car: '#34d399', truck: '#a78bfa', bus: '#f59e0b', motorcycle: '#f472b6', bicycle: '#fb923c', face: '#facc15' };
export const colorFor = (l: string) => COLORS[l] || '#22d3ee';

// SVG overlay (viewBox 0..1000 x 0..1000*h/w): detections, rule zones/lines, privacy masks
export function Overlay(p: { dets?: Det[]; rules?: any[]; masks?: any[]; aspect?: number }) {
  const W = 1000, H = Math.round(1000 / (p.aspect || 16 / 9));
  return (
    <svg class="ov" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none">
      {(p.masks || []).filter((m) => m.enabled !== false).map((m) => (
        <rect x={m.x * W} y={m.y * H} width={m.w * W} height={m.h * H} fill="rgba(0,0,0,.65)" stroke="#64748b" stroke-dasharray="6 4" />
      ))}
      {(p.rules || []).filter((r) => r.enabled !== false).map((r) => (
        <g>
          {r.zone?.length >= 3 && (
            <polygon points={r.zone.map((q: number[]) => `${q[0] * W},${q[1] * H}`).join(' ')} fill="rgba(56,189,248,.10)" stroke="#38bdf8" stroke-width="2" />
          )}
          {r.line?.length === 2 && (
            <>
              <line x1={r.line[0][0] * W} y1={r.line[0][1] * H} x2={r.line[1][0] * W} y2={r.line[1][1] * H} stroke="#f43f5e" stroke-width="3" />
              <text x={r.line[0][0] * W + 6} y={r.line[0][1] * H - 6} fill="#f43f5e" font-size="18" font-weight="700">A</text>
              <text x={r.line[1][0] * W + 6} y={r.line[1][1] * H - 6} fill="#f43f5e" font-size="18" font-weight="700">B</text>
            </>
          )}
          {(r.zone?.length >= 3 || r.line?.length === 2) && (
            <text x={(r.zone?.[0]?.[0] ?? r.line[0][0]) * W + 6} y={(r.zone?.[0]?.[1] ?? r.line[0][1]) * H + 20} fill="#e2e8f0" font-size="16" font-weight="600" style={{ paintOrder: 'stroke', stroke: '#000', strokeWidth: 3 }}>{r.name}</text>
          )}
        </g>
      ))}
      {(p.dets || []).map((d) => {
        const c = colorFor(d.label);
        const [x1, y1, x2, y2] = d.box;
        const extra = Object.values(d.attrs || {}).join(' ');
        const label = `${d.label} #${d.id}${extra ? ' · ' + extra : ''}`;
        return (
          <g>
            <rect x={x1 * W} y={y1 * H} width={(x2 - x1) * W} height={(y2 - y1) * H} fill="none" stroke={c} stroke-width="3" rx="4" />
            <rect x={x1 * W} y={Math.max(0, y1 * H - 24)} width={Math.min(W - x1 * W, label.length * 9.5 + 12)} height="22" fill={c} rx="3" />
            <text x={x1 * W + 6} y={Math.max(0, y1 * H - 24) + 16} fill="#0b1220" font-size="15" font-weight="700">{label}</text>
            {d.part && <rect x={d.part[0] * W} y={d.part[1] * H} width={(d.part[2] - d.part[0]) * W} height={(d.part[3] - d.part[1]) * H} fill="none" stroke="#facc15" stroke-width="3" />}
          </g>
        );
      })}
    </svg>
  );
}

// Live player with overlays and controls
export function LiveViewer(p: { stream: 'main' | 'sub'; showAi?: boolean; rules?: any[]; masks?: any[]; onStats?: (s: PlayerStats) => void; compact?: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const video = useRef<HTMLVideoElement>(null);
  const box = useRef<HTMLDivElement>(null);
  const player = useRef<LivePlayer | null>(null);
  const [state, setState] = useState<{ s: string; msg?: string }>({ s: 'connecting' });
  const [dets, setDets] = useState<Det[]>([]);
  const [stats, setStats] = useState<PlayerStats | null>(null);
  const [aspect, setAspect] = useState(16 / 9);
  const lastDet = useRef(0);
  const detBuf = useRef<{ pts: number; objects: Det[] }[]>([]);

  useEffect(() => {
    const pl = new LivePlayer(canvas.current!, video.current!, (s, msg) => setState({ s, msg }), (st) => {
      setStats(st);
      if (st.width && st.height) setAspect(st.width / st.height);
      p.onStats?.(st);
    });
    player.current = pl;
    pl.start(p.stream);
    return () => pl.stop();
  }, [p.stream]);

  useEffect(() => {
    if (!p.showAi) {
      setDets([]);
      return;
    }
    // boxes arrive over a separate socket and the video plays slightly behind: keep the last few detection sets and show the
    // one captured at the same moment as the picture on screen
    let shown: any = null;
    const un = onBus('detections', (m) => {
      lastDet.current = Date.now();
      detBuf.current.push({ pts: m.pts_us || 0, objects: m.objects || [] });
      if (detBuf.current.length > 40) detBuf.current.shift();
    });
    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const buf = detBuf.current;
      if (!buf.length) return;
      let pick = buf[buf.length - 1];
      const cur = player.current?.currentPts();
      if (cur) {
        let best = Infinity;
        for (const e of buf) {
          const d = Math.abs(e.pts - cur);
          if (e.pts && d < best) (best = d), (pick = e);
        }
        if (best > 1.5e6) pick = buf[buf.length - 1];  // different clocks: do not hide the boxes, show the newest
      }
      if (pick !== shown) {
        shown = pick;
        setDets(pick.objects);
      }
    };
    raf = requestAnimationFrame(tick);
    const t = setInterval(() => {
      if (Date.now() - lastDet.current > 1500) {
        detBuf.current = [];
        shown = null;
        setDets([]);
      }
    }, 500);
    return () => {
      un();
      cancelAnimationFrame(raf);
      clearInterval(t);
    };
  }, [p.showAi]);

  const snapshot = async () => {
    try {
      const r = await fetch('/api/snapshot.jpg', { headers: { 'X-PicoCam-UI': '1' } });
      if (!r.ok) throw new Error('snapshot failed');
      const b = await r.blob();
      const a = document.createElement('a');
      a.href = URL.createObjectURL(b);
      a.download = `picocam-${new Date().toISOString().replace(/[:.]/g, '-')}.jpg`;
      a.click();
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  const full = () => (document.fullscreenElement ? document.exitFullscreen() : box.current?.requestFullscreen());
  const mse = stats?.mode === 'MSE';

  return (
    <div class="player-wrap">
    <div class="player" ref={box} style={{ aspectRatio: String(aspect) }}>
      <canvas ref={canvas} class={mse ? 'hidden' : ''} />
      <video ref={video} class={mse ? '' : 'hidden'} muted playsInline autoplay />
      <Overlay dets={dets} rules={p.rules} masks={p.masks} aspect={aspect} />
      {state.s !== 'playing' && (
        <div class="msg">
          <div>
            {state.s === 'connecting' ? <span class="spin" /> : <Icon n="warn" s={28} />}
            <div class="small" style={{ marginTop: 8 }}>{state.msg || (state.s === 'connecting' ? 'Connecting to the camera…' : '')}</div>
          </div>
        </div>
      )}
      <div class="ctl">
        <span class="grow" />
        <Btn sm icon="camera" title="Snapshot" onClick={snapshot} />
        <Btn sm icon="refresh" title="Reconnect" onClick={() => player.current?.start(p.stream)} />
        <Btn sm icon="full" title="Fullscreen" onClick={full} />
      </div>
    </div>
    {/* status badges sit below the picture so they never cover the camera's own time/name overlay */}
      <div class="hudbar">
        <span class="badge"><span class="dot live" /> LIVE</span>
        {stats && !p.compact && (
          <>
            <span class="badge">{stats.width}×{stats.height}</span>
            <span class="badge">{stats.fps} fps</span>
            <span class="badge">{stats.kbps} kbps</span>
            <span class="badge" title="Playback buffer">{stats.mode} · {stats.delayMs} ms</span>
          </>
        )}
      </div>
    </div>
  );
}

// Still picture of the camera (for editors): refreshes on demand
export function useSnapshotUrl(): [string, () => void] {
  const [url, setUrl] = useState('');
  const load = () => {
    fetch('/api/snapshot.jpg?' + Date.now(), { headers: { 'X-PicoCam-UI': '1' } })
      .then((r) => (r.ok ? r.blob() : Promise.reject()))
      .then((b) => setUrl((old) => {
        if (old) URL.revokeObjectURL(old);
        return URL.createObjectURL(b);
      }), () => {});
  };
  useEffect(load, []);
  return [url, load];
}

export const loadRulesAndMasks = () => Promise.all([api.get('/api/config/rules'), api.get('/api/config/masks')]);
