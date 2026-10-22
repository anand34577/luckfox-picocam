import { useEffect, useState } from 'preact/hooks';
import { api, onAuthLost } from './api';
import { AiPage } from './pages/ai';
import { DashboardPage } from './pages/dashboard';
import { EventsPage } from './pages/events';
import { ImagePage } from './pages/image';
import { IntegrationsPage } from './pages/integrations';
import { LivePage } from './pages/live';
import { NetworkPage } from './pages/network';
import { PlaybackPage } from './pages/playback';
import { StoragePage } from './pages/storage';
import { SystemPage } from './pages/system';
import { VideoPage } from './pages/video';
import { busConnected, connectBus, disconnectBus, session, status, theme, useStore } from './store';
import { Icon } from './ui/icons';
import { Btn, Field, Input, toast } from './ui/kit';
import { Wizard } from './pages/wizard';

interface Route {
  path: string;
  title: string;
  icon: string;
  group: string;
  page: () => any;
  role?: 'operator' | 'admin';
  sub?: string;
}

const ROUTES: Route[] = [
  { path: 'live', title: 'Live view', icon: 'live', group: 'Monitor', page: LivePage, sub: 'Low-latency live video with AI overlays' },
  { path: 'events', title: 'Events', icon: 'events', group: 'Monitor', page: EventsPage, sub: 'Detections, alarms and plate reads' },
  { path: 'playback', title: 'Playback', icon: 'playback', group: 'Monitor', page: PlaybackPage, sub: 'Recordings on the SD card' },
  { path: 'dashboard', title: 'Dashboard', icon: 'dashboard', group: 'Monitor', page: DashboardPage, sub: 'Health and activity at a glance' },
  { path: 'ai', title: 'AI & Analytics', icon: 'ai', group: 'Configure', page: AiPage, role: 'operator', sub: 'Models, pipelines, zones and rules on the NPU' },
  { path: 'video', title: 'Video & Audio', icon: 'video', group: 'Configure', page: VideoPage, role: 'operator', sub: 'Encoders, OSD and privacy masks' },
  { path: 'image', title: 'Image', icon: 'image', group: 'Configure', page: ImagePage, role: 'operator', sub: 'Smart exposure, WDR, white balance, noise' },
  { path: 'storage', title: 'Storage & Recording', icon: 'storage', group: 'Configure', page: StoragePage, role: 'operator', sub: 'SD card, schedule and retention' },
  { path: 'integrations', title: 'Integrations', icon: 'plug', group: 'Configure', page: IntegrationsPage, role: 'operator', sub: 'RTSP, ONVIF, webhooks, MQTT, API' },
  { path: 'network', title: 'Network & Time', icon: 'network', group: 'Configure', page: NetworkPage, role: 'admin', sub: 'Addresses, DNS, NTP and time zone' },
  { path: 'system', title: 'System', icon: 'system', group: 'Configure', page: SystemPage, role: 'operator', sub: 'Users, firmware, performance and logs' },
];

const ROLE_RANK: Record<string, number> = { none: 0, viewer: 1, operator: 2, admin: 3 };

function useHash() {
  const [h, setH] = useState(location.hash.replace(/^#\/?/, ''));
  useEffect(() => {
    const f = () => setH(location.hash.replace(/^#\/?/, ''));
    addEventListener('hashchange', f);
    return () => removeEventListener('hashchange', f);
  }, []);
  return h;
}

export function App() {
  const s = useStore(session);
  useEffect(() => {
    const load = () => api.get('/api/session').then((v) => session.set(v), () => session.set({ activated: true, role: 'none' } as any));
    load();
    // a 401 from one call does not prove the session is gone: ask the server, and only then drop the live feed
    onAuthLost(() => {
      api.get('/api/session').then(
        (v) => {
          if (v.role === 'none') disconnectBus();
          session.set(v);
        },
        () => {},  // camera unreachable: keep the screen as it is
      );
    });
  }, []);
  if (!s) return <div class="login-wrap"><span class="spin" /></div>;
  if (!s.activated) return <Activate />;
  if (s.role === 'none') return <Login />;
  return <Shell />;
}

function Shell() {
  const s = useStore(session)!;
  const st = useStore(status);
  const live = useStore(busConnected);
  const th = useStore(theme);
  const hash = useHash();
  const [open, setOpen] = useState(false);
  const [wizard, setWizard] = useState(s.role === 'admin' && localStorage.getItem('pc-wizard') === 'pending');
  const [upd, setUpd] = useState<any>(null);
  const [ack, setAck] = useState(localStorage.getItem('pc-ack-update') || '');
  useEffect(() => {
    api.get('/api/system/update-state').then(setUpd, () => {});
  }, []);
  useEffect(() => {
    connectBus();
    // the board has no battery-backed clock: if it is clearly wrong, set it from this browser
    api.get('/api/status').then((v) => {
      status.set({ ...v, topic: 'status' });
      const t = v?.system?.time_ms || 0;
      if (s.role === 'admin' && (t < 1700000000000 || Math.abs(t - Date.now()) > 120000) && !v.system.ntp_synced)
        api.post('/api/system/time', { epoch_ms: Date.now() }).then(() => toast('Camera clock set from this browser', 'ok'), () => {});
    }, () => {});
  }, []);
  const allowed = ROUTES.filter((r) => ROLE_RANK[s.role] >= ROLE_RANK[r.role || 'viewer']);
  const route = allowed.find((r) => hash.split(/[/?]/)[0] === r.path) || allowed[0];
  const Page = route.page;
  const groups = [...new Set(allowed.map((r) => r.group))];
  const logout = async () => {
    await api.post('/api/logout').catch(() => {});
    disconnectBus();
    session.set({ ...s, role: 'none', user: '' });
  };
  return (
    <div class="shell">
      <aside class={'side' + (open ? ' open' : '')} onClick={() => setOpen(false)}>
        <div class="brand">
          <div class="brand-logo"><Icon n="camera" s={18} w={2} /></div>
          <div>
            <b>{s.device || 'PicoCam'}</b>
            <small>{s.model}</small>
          </div>
        </div>
        {groups.map((g) => (
          <>
            <div class="nav-group">{g}</div>
            <nav class="nav">
              {allowed.filter((r) => r.group === g).map((r) => (
                <a href={'#/' + r.path} class={r === route ? 'on' : ''}>
                  <Icon n={r.icon} />
                  {r.title}
                </a>
              ))}
            </nav>
          </>
        ))}
        <div class="side-foot">
          <div class="row" style={{ gap: 8 }}>
            <span class={'dot ' + (live ? 'ok' : 'bad')} />
            <span>{live ? 'Connected' : 'Reconnecting…'}</span>
            <span class="grow" />
            <span>v{s.firmware}</span>
          </div>
          {st?.system && (
            <div class="row" style={{ gap: 10 }}>
              <span title="CPU"><Icon n="cpu" s={12} /> {st.system.cpu_pct}%</span>
              <span title="Temperature"><Icon n="thermo" s={12} /> {st.system.temp_c}°C</span>
              <span title="Free memory"><Icon n="memory" s={12} /> {Math.round(st.system.mem_avail_kb / 1024)} MB</span>
            </div>
          )}
        </div>
      </aside>
      <div class="main">
        <header class="topbar">
          <button class="btn icon ghost menu-btn" onClick={() => setOpen(!open)}><Icon n="menu" /></button>
          <div>
            <h1>{route.title}</h1>
            <div class="sub">{route.sub}</div>
          </div>
          <span class="grow" />
          <Btn kind="ghost" icon={th === 'dark' ? 'sun' : 'moon'} title="Theme" onClick={() => theme.set(th === 'dark' ? 'light' : 'dark')} />
          <span class="badge"><Icon n="user" s={12} /> {s.user} · {s.role}</span>
          <Btn kind="ghost" icon="logout" title="Sign out" onClick={logout} />
        </header>
        <main class="page">
          {st?.network_pending_s > 0 && (
            <div class="banner warn">
              <Icon n="network" s={16} />
              <span>Network settings were changed. They revert in <b>{st.network_pending_s} s</b> unless you confirm that this page still works.</span>
              <Btn kind="primary" sm onClick={() => api.post('/api/system/network/confirm').then(() => toast('Network settings confirmed', 'ok'), (e) => toast(e.message, 'err'))}>Keep new settings</Btn>
            </div>
          )}
          {upd && (upd.rolled_back || upd.config_recovered) && ack !== JSON.stringify([upd.rolled_back, upd.config_recovered]) && (
            <div class="banner warn">
              <Icon n="warn" s={16} />
              <span>{upd.rolled_back ? 'The last firmware update kept crashing, so the camera went back to the previous version automatically.' : 'The camera kept crashing with the last settings and restored the last configuration that ran well.'}</span>
              <Btn sm onClick={() => {
                const v = JSON.stringify([upd.rolled_back, upd.config_recovered]);
                localStorage.setItem('pc-ack-update', v);
                setAck(v);
              }}>Dismiss</Btn>
            </div>
          )}
          <Page />
        </main>
        {wizard && <Wizard onClose={() => { localStorage.setItem('pc-wizard', 'done'); setWizard(false); }} />}
      </div>
    </div>
  );
}

function Login() {
  const s = useStore(session)!;
  const [u, setU] = useState('admin');
  const [p, setP] = useState('');
  const [err, setErr] = useState('');
  const submit = async (e: Event) => {
    e.preventDefault();
    setErr('');
    try {
      const r = await api.post('/api/login', { username: u, password: p });
      session.set({ ...s, user: r.user, role: r.role });
    } catch (x: any) {
      setErr(x.message);
    }
  };
  return (
    <div class="login-wrap">
      <form class="card login" onSubmit={submit}>
        <div class="brand" style={{ padding: '0 0 20px' }}>
          <div class="brand-logo"><Icon n="camera" s={18} w={2} /></div>
          <div>
            <b>{s.device || 'PicoCam'}</b>
            <small>{s.model} · v{s.firmware}</small>
          </div>
        </div>
        <div class="form">
          <Field label="User name"><Input value={u} onInput={setU} /></Field>
          <Field label="Password"><Input type="password" value={p} onInput={setP} /></Field>
          {err && <div class="callout bad">{err}</div>}
          <button class="btn primary" type="submit">Sign in</button>
        </div>
      </form>
    </div>
  );
}

function Activate() {
  const s = useStore(session)!;
  const [p, setP] = useState('');
  const [p2, setP2] = useState('');
  const [err, setErr] = useState('');
  const strong = p.length >= 8 && /[a-z]/i.test(p) && /\d/.test(p);
  const submit = async (e: Event) => {
    e.preventDefault();
    if (p !== p2) return setErr('Passwords do not match');
    try {
      const r = await api.post('/api/activate', { password: p });
      localStorage.setItem('pc-wizard', 'pending');  // the first-run guide opens once
      session.set({ ...s, activated: true, user: r.user, role: r.role });
    } catch (x: any) {
      setErr(x.message);
    }
  };
  return (
    <div class="login-wrap">
      <form class="card login" onSubmit={submit}>
        <div class="brand" style={{ padding: '0 0 10px' }}>
          <div class="brand-logo"><Icon n="shield" s={18} w={2} /></div>
          <div>
            <b>Activate your camera</b>
            <small>{s.model}</small>
          </div>
        </div>
        <p class="muted small">Set a password for the <b>admin</b> account. It protects the web portal, RTSP streams, ONVIF and the camera’s shell (SSH).</p>
        <div class="form">
          <Field label="Admin password" help="At least 8 characters with letters and digits"><Input type="password" value={p} onInput={setP} /></Field>
          <Field label="Confirm password"><Input type="password" value={p2} onInput={setP2} /></Field>
          {err && <div class="callout bad">{err}</div>}
          <button class="btn primary" type="submit" disabled={!strong}>Activate</button>
        </div>
      </form>
    </div>
  );
}
