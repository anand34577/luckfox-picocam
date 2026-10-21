// App-wide state: session, live status and the realtime event bus (/ws/events).
import { useEffect, useState } from 'preact/hooks';

type Sub<T> = (v: T) => void;

export class Store<T> {
  private subs = new Set<Sub<T>>();
  constructor(public value: T) {}
  set(v: T) {
    this.value = v;
    this.subs.forEach((s) => s(v));
  }
  sub(f: Sub<T>) {
    this.subs.add(f);
    return () => this.subs.delete(f);
  }
}

export function useStore<T>(s: Store<T>): T {
  const [v, setV] = useState(s.value);
  useEffect(() => {
    setV(s.value);
    const un = s.sub(setV);
    return () => {
      un();
    };
  }, [s]);
  return v;
}

export interface Session {
  activated: boolean;
  user: string;
  role: 'admin' | 'operator' | 'viewer' | 'none';
  device: string;
  model: string;
  firmware: string;
  ip?: string;  // the address this browser connects from
}

export const session = new Store<Session | null>(null);
export const status = new Store<any>(null);
export const busConnected = new Store(false);

// bus: topic listeners
const listeners = new Map<string, Set<(m: any) => void>>();
export function onBus(topic: string, f: (m: any) => void) {
  if (!listeners.has(topic)) listeners.set(topic, new Set());
  listeners.get(topic)!.add(f);
  return () => {
    listeners.get(topic)!.delete(f);
  };
}
export function useBus(topic: string, f: (m: any) => void, deps: any[] = []) {
  useEffect(() => onBus(topic, f), deps);
}

let ws: WebSocket | null = null;
let retry = 0;
export function connectBus() {
  if (ws) return;
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws/events`);
  ws.onopen = () => {
    retry = 0;
    busConnected.set(true);
  };
  ws.onmessage = (e) => {
    try {
      const m = JSON.parse(e.data);
      if (m.topic === 'status') status.set(m);
      listeners.get(m.topic)?.forEach((f) => f(m));
    } catch {}
  };
  ws.onclose = () => {
    ws = null;
    busConnected.set(false);
    if (session.value?.activated && session.value.role !== 'none') setTimeout(connectBus, Math.min(10000, 1000 * ++retry));
  };
}
export function disconnectBus() {
  const w = ws;
  ws = null;
  w?.close();
}

export const canEdit = () => ['admin', 'operator'].includes(session.value?.role || '');
export const isAdmin = () => session.value?.role === 'admin';

// theme
export const theme = new Store<string>(localStorage.getItem('pc-theme') || 'dark');
theme.sub((t) => {
  document.documentElement.dataset.theme = t;
  try {
    localStorage.setItem('pc-theme', t);
  } catch {}
});
document.documentElement.dataset.theme = theme.value;
