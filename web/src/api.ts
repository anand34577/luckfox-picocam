import { useEffect, useRef, useState } from 'react';

export interface EventRow {
  id: number;
  ts_ms: number;
  device_id: string;
  type: 'vehicle' | 'face';
  track_id: number;
  vehicle_type?: string;
  plate_text?: string;
  plate_valid: boolean;
  plate_score?: number;
  person_id?: number;
  person_name?: string;
  quality?: number;
  frame_url?: string;
  crop_url?: string;
  extra_url?: string;
  meta?: Record<string, unknown>;
}

export interface DeviceStatus {
  ip?: string;
  stream_fps?: number;
  ai_fps?: number;
  ai_ms?: number;
  width?: number;
  height?: number;
  cma_used_kb?: number;
  cma_total_kb?: number;
  mem_avail_kb?: number;
  streams?: { clean?: string; overlay?: string };
  stats?: Record<string, number>;
}

export interface Device {
  id: string;
  profile: 'anpr' | 'face' | string;
  ip: string;
  last_seen: number;
  online: boolean;
  status: DeviceStatus;
  clean_path: string;
  overlay_path: string;
}

export interface Person {
  id: number;
  name: string;
  first_seen: number;
  last_seen: number;
  visits: number;
  cover_url: string;
}

export interface Stats {
  vehicles_today: number;
  plates_today: number;
  faces_today: number;
  persons_total: number;
  new_persons_today: number;
  by_vehicle_type: Record<string, number>;
}

export async function api<T>(path: string, init?: RequestInit): Promise<T> {
  const r = await fetch(path, {
    ...init,
    headers: init?.body ? { 'Content-Type': 'application/json', ...init.headers } : init?.headers,
  });
  if (!r.ok) throw new Error(`${r.status} ${await r.text()}`);
  return r.json() as Promise<T>;
}

export function personLabel(id?: number, name?: string) {
  if (!id) return 'Unknown';
  return name ? name : `ID ${id}`;
}

type LiveMsg = { kind: 'event'; data: EventRow } | { kind: 'device'; data: Device };

/** Subscribes to the server's live feed; reconnects automatically. */
export function useLive(onMsg: (m: LiveMsg) => void) {
  const cb = useRef(onMsg);
  cb.current = onMsg;
  const [connected, setConnected] = useState(false);
  useEffect(() => {
    let ws: WebSocket | null = null;
    let timer: number | undefined;
    let stopped = false;
    const connect = () => {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      ws = new WebSocket(`${proto}://${location.host}/api/v1/ws`);
      ws.onopen = () => setConnected(true);
      ws.onmessage = (e) => {
        try {
          cb.current(JSON.parse(e.data));
        } catch {
          /* ignore malformed */
        }
      };
      ws.onclose = () => {
        setConnected(false);
        if (!stopped) timer = window.setTimeout(connect, 2000);
      };
    };
    connect();
    return () => {
      stopped = true;
      window.clearTimeout(timer);
      ws?.close();
    };
  }, []);
  return connected;
}

/** Devices list kept fresh by heartbeats over the live feed. */
export function useDevices() {
  const [devices, setDevices] = useState<Device[]>([]);
  useEffect(() => {
    api<Device[]>('/api/v1/devices').then(setDevices).catch(() => {});
    const t = window.setInterval(() => api<Device[]>('/api/v1/devices').then(setDevices).catch(() => {}), 15000);
    return () => window.clearInterval(t);
  }, []);
  const connected = useLive((m) => {
    if (m.kind !== 'device') return;
    setDevices((ds) => {
      const d = { ...m.data, online: true };
      const i = ds.findIndex((x) => x.id === d.id);
      if (i < 0) return [...ds, d].sort((a, b) => a.id.localeCompare(b.id));
      const copy = ds.slice();
      copy[i] = d;
      return copy;
    });
  });
  return { devices, connected };
}

let webrtcBase: Promise<string> | null = null;
export function getWebrtcBase() {
  webrtcBase ??= api<{ webrtc_base: string }>('/api/v1/config').then((c) => c.webrtc_base);
  return webrtcBase;
}
