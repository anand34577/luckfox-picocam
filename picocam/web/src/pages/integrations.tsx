import { useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { isAdmin, status, useStore } from '../store';
import { Badge, Btn, Callout, Card, confirmDialog, Copy, Empty, Field, Input, Modal, Num, SaveBar, Seg, Select, Tabs, toast, ToggleRow, useSection } from '../ui/kit';

const EVENT_TYPES = ['intrusion', 'loitering', 'line_cross', 'count', 'presence', 'track', 'motion', 'test'];

function Streaming() {
  const rtsp = useSection<any>('rtsp');
  const onvif = useSection<any>('onvif');
  const st = useStore(status);
  const ip = location.hostname;
  if (!rtsp.val || !onvif.val) return <div class="empty"><span class="spin" /></div>;
  const port = rtsp.val.port === 554 ? '' : `:${rtsp.val.port}`;
  return (
    <div class="grid g2">
      <Card title="RTSP" actions={<Badge kind="ok">{st?.rtsp_sessions ?? 0} active</Badge>}>
        <div class="form">
          <ToggleRow title="RTSP server" checked={rtsp.val.enabled !== false} onChange={(x) => rtsp.set({ enabled: x })} />
          <div class="form g2">
            <Field label="Port"><Num value={rtsp.val.port} min={1} max={65535} onChange={(x) => rtsp.set({ port: x })} /></Field>
            <Field label="Authentication"><Select value={rtsp.val.auth} onChange={(x) => rtsp.set({ auth: x })} options={[['digest', 'Digest (recommended)'], ['basic', 'Basic'], ['none', 'None (open)']]} /></Field>
          </div>
          {rtsp.val.auth === 'none' && <Callout kind="warn">Anyone on the network can watch the streams without a password.</Callout>}
          <div class="section-title">Stream addresses</div>
          <Copy text={`rtsp://${ip}${port}/live/main`} />
          <Copy text={`rtsp://${ip}${port}/live/sub`} />
          <div class="small dim">Also accepted: /Streaming/Channels/101 · 102, /h264/ch1/main/av_stream, /cam/realmonitor?subtype=0|1, /live/0 · 1. TCP and UDP transports.</div>
          <div class="small dim">VLC low latency: Tools › Preferences › Input › Network caching 150 ms, or <code>ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp URL</code>.</div>
        </div>
        <SaveBar dirty={rtsp.dirty} onSave={() => rtsp.save()} onReset={rtsp.reload} />
      </Card>
      <Card title="ONVIF (NVR / VMS)">
        <div class="form">
          <ToggleRow title="ONVIF Profile S" desc="Hikvision, Dahua, Uniview, Synology, Milestone, Blue Iris, Frigate, Home Assistant…" checked={onvif.val.enabled !== false} onChange={(x) => onvif.set({ enabled: x })} />
          <ToggleRow title="Discoverable (WS-Discovery)" checked={onvif.val.discovery !== false} onChange={(x) => onvif.set({ discovery: x })} />
          <Field label="Device service"><Copy text={`http://${location.host}/onvif/device_service`} /></Field>
          <Field label="Snapshot"><Copy text={`http://${location.host}/snapshot.jpg`} /></Field>
          <Callout>Add the camera in your NVR as ONVIF with port {location.port || 80} and a camera user. Motion and AI events are delivered as ONVIF motion alarms.</Callout>
        </div>
        <SaveBar dirty={onvif.dirty} onSave={() => onvif.save()} onReset={onvif.reload} />
      </Card>
    </div>
  );
}

// ---- event filters shared by every delivery target
function Filters(p: { v: any; set: (k: string, x: any) => void; rules: any[] }) {
  const v = p.v;
  const toggle = (k: string, id: string) => p.set(k, (v[k] || []).includes(id) ? v[k].filter((x: string) => x !== id) : [...(v[k] || []), id]);
  return (
    <>
      <Field label="Event types" help="Empty = all">
        <div class="chips">{EVENT_TYPES.map((t) => <span class={'chip' + ((v.types || []).includes(t) ? ' on' : '')} onClick={() => toggle('types', t)}>{t}</span>)}</div>
      </Field>
      {p.rules.length > 0 && (
        <Field label="Only these rules" help="Empty = every rule">
          <div class="chips">{p.rules.map((r) => <span class={'chip' + ((v.rules || []).includes(r.id) ? ' on' : '')} onClick={() => toggle('rules', r.id)}>{r.name}</span>)}</div>
        </Field>
      )}
      <Field label="Only these objects" help="Comma separated, e.g. person, car. Empty = all">
        <Input value={(v.labels || []).join(', ')} onInput={(x) => p.set('labels', x.split(',').map((y) => y.trim()).filter(Boolean))} />
      </Field>
    </>
  );
}

function TlsFields(p: { v: any; set: (k: string, x: any) => void }) {
  return (
    <>
      <Field label="Certificate check" help="Trusted CA: public services. Pin: your own server with a self-signed certificate."><Select value={p.v.verify || 'system'} onChange={(x) => p.set('verify', x)} options={[['system', 'Trusted CA'], ['pin', 'Pin fingerprint'], ['none', 'Skip (insecure)']]} /></Field>
      {p.v.verify === 'pin' && <Field label="SHA-256 fingerprint"><Input mono value={p.v.pin} onInput={(x) => p.set('pin', x)} placeholder="ab:cd:…" /></Field>}
    </>
  );
}

function WebhookEdit(p: { w: any; rules: any[]; onSave: (w: any) => void; onClose: () => void }) {
  const [w, setW] = useState<any>({ ...p.w });
  const set = (k: string, v: any) => setW({ ...w, [k]: v });
  const hdrs = Object.entries(w.headers || {}).map(([k, v]) => `${k}: ${v}`).join('\n');
  const https = (w.url || '').startsWith('https://');
  return (
    <Modal title="Webhook" onClose={p.onClose} footer={<><Btn kind="ghost" onClick={p.onClose}>Cancel</Btn><Btn kind="primary" disabled={!/^https?:\/\//.test(w.url || '')} onClick={() => p.onSave(w)}>Save</Btn></>}>
      <div class="form">
        <Field label="Name"><Input value={w.name} onInput={(v) => set('name', v)} placeholder="e.g. Gate controller" /></Field>
        <Field label="URL" help="http:// or https://. user:password@host for basic auth"><Input value={w.url} onInput={(v) => set('url', v)} placeholder="https://example.com/hook" /></Field>
        {https && <TlsFields v={w} set={set} />}
        <div class="form g2">
          <Field label="Method"><Seg value={w.method || 'POST'} onChange={(v) => set('method', v)} options={['POST', 'PUT']} /></Field>
          <Field label="Pictures"><Select value={w.include_image || 'none'} onChange={(v) => set('include_image', v)} options={[['none', 'None'], ['snapshot', 'Snapshot (base64)'], ['all', 'Snapshot + crops']]} /></Field>
        </div>
        <Field label="Signing secret" help="Optional. Each request carries X-PicoCam-Signature: sha256=HMAC(secret, timestamp + '.' + body), with X-PicoCam-Timestamp and X-PicoCam-Event-Id.">
          <Input type="password" value={w.secret} onInput={(v) => set('secret', v)} />
        </Field>
        <Filters v={w} set={set} rules={p.rules} />
        <Field label="Extra headers" help="One per line: Name: value">
          <textarea class="input" value={hdrs} onInput={(e) => {
            const o: any = {};
            (e.target as HTMLTextAreaElement).value.split('\n').forEach((l) => {
              const i = l.indexOf(':');
              if (i > 0) o[l.slice(0, i).trim()] = l.slice(i + 1).trim();
            });
            set('headers', o);
          }} />
        </Field>
        <ToggleRow title="Enabled" checked={w.enabled !== false} onChange={(v) => set('enabled', v)} />
      </div>
    </Modal>
  );
}

function GpioEdit(p: { g: any; rules: any[]; onSave: (g: any) => void; onClose: () => void }) {
  const [g, setG] = useState<any>({ ...p.g });
  const set = (k: string, v: any) => setG({ ...g, [k]: v });
  return (
    <Modal title="Relay / lamp output" onClose={p.onClose} footer={<><Btn kind="ghost" onClick={p.onClose}>Cancel</Btn><Btn kind="primary" disabled={g.gpio === undefined || g.gpio < 0} onClick={() => p.onSave(g)}>Save</Btn></>}>
      <div class="form">
        <Callout>Pulses a GPIO pin when a matching event happens: open a gate for an allowed plate, switch on a lamp or siren. Wire a relay or transistor to the pin, never a load directly.</Callout>
        <Field label="Name"><Input value={g.name} onInput={(v) => set('name', v)} placeholder="e.g. Gate relay" /></Field>
        <div class="form g2">
          <Field label="GPIO number"><Num value={g.gpio ?? -1} min={0} max={511} onChange={(v) => set('gpio', v)} /></Field>
          <Field label="Pulse length"><Num value={g.pulse_ms ?? 1000} min={50} max={60000} unit="ms" onChange={(v) => set('pulse_ms', v)} /></Field>
        </div>
        <ToggleRow title="Active high" desc="Pin goes high during the pulse (off: it goes low)" checked={g.active_high !== false} onChange={(v) => set('active_high', v)} />
        <Filters v={g} set={set} rules={p.rules} />
        <ToggleRow title="Enabled" checked={g.enabled !== false} onChange={(v) => set('enabled', v)} />
      </div>
    </Modal>
  );
}

function Events() {
  const { val, set, save, dirty, reload } = useSection<any>('integrations');
  const [edit, setEdit] = useState<any>(null);
  const [gedit, setGedit] = useState<any>(null);
  const [ist, setIst] = useState<any>(null);
  const [rules, setRules] = useState<any[]>([]);
  useEffect(() => {
    const l = () => api.get('/api/integrations/status').then(setIst, () => {});
    l();
    api.get('/api/config/rules').then((r) => setRules(r.filter((x: any) => x.type !== 'motion')), () => {});
    const t = setInterval(l, 4000);
    return () => clearInterval(t);
  }, []);
  if (!val) return <div class="empty"><span class="spin" /></div>;
  const mq = val.mqtt || {}, ea = val.edgeai_server || {}, ftp = val.ftp || {}, sm = val.smtp || {}, tg = val.telegram || {};
  const hooks: any[] = val.webhooks || [], gpios: any[] = val.gpio || [];
  const stat = (n: string) => {
    const s = ist?.integrations?.[n];
    if (!s) return null;
    return <>{s.queued > 0 && <Badge kind="warn">{s.queued} waiting</Badge>} {s.seen && <Badge kind={s.ok ? 'ok' : 'bad'}>{s.ok ? 'delivered' : s.error}</Badge>}</>;
  };
  const f = (key: string, cur: any) => (k: string, v: any) => set({ [key]: { ...cur, [k]: v } });
  const sMq = f('mqtt', mq), sEa = f('edgeai_server', ea), sFtp = f('ftp', ftp), sSm = f('smtp', sm), sTg = f('telegram', tg);
  return (
    <div class="col">
      {ist?.queued > 0 && <Callout kind="warn">{ist.queued} event(s) are waiting to be delivered. They are kept on the camera and retried automatically (up to 24 hours); nothing is lost if the network or a server is down.</Callout>}
      {ist?.delivery_dropped > 0 && <Callout kind="bad">{ist.delivery_dropped} event(s) could not be delivered and were dropped (too old, rejected by the server or the queue was full).</Callout>}
      <div class="grid g2">
        <Card title="Webhooks" hint="HTTP(S) POST of every event as JSON" tight actions={<Btn sm icon="plus" onClick={() => setEdit({ name: '', url: 'https://', enabled: true, include_image: 'none', types: [] })}>Add</Btn>}>
          {!hooks.length && <Empty icon="link">No webhooks</Empty>}
          <table class="table"><tbody>
            {hooks.map((w, i) => (
              <tr class="click" tabIndex={0} onClick={() => setEdit({ ...w, _i: i })}>
                <td><b>{w.name || 'Webhook'}</b><div class="mono dim small">{w.url}</div></td>
                <td class="right" onClick={(e) => e.stopPropagation()}>{stat('webhook:' + (w.name || w.url))} {w.enabled === false && <Badge>off</Badge>}
                  <Btn sm kind="ghost" icon="trash" onClick={() => set({ webhooks: hooks.filter((_, j) => j !== i) })} /></td>
              </tr>
            ))}
          </tbody></table>
        </Card>
        <Card title="MQTT" actions={<>{stat('mqtt')} {ist?.mqtt?.enabled && <Badge kind={ist.mqtt.connected ? 'ok' : 'bad'}>{ist.mqtt.connected ? (ist.mqtt.tls ? 'connected (TLS)' : 'connected') : ist.mqtt.error || 'disconnected'}</Badge>}</>}>
          <div class="form">
            <ToggleRow title="Publish events to MQTT" desc="Home Assistant, Node-RED, any broker. Delivery is acknowledged (QoS 1) and retried." checked={!!mq.enabled} onChange={(x) => sMq('enabled', x)} />
            <div class="form g2">
              <Field label="Broker host"><Input value={mq.host} onInput={(x) => sMq('host', x)} placeholder="192.168.1.5" /></Field>
              <Field label="Port"><Num value={mq.port ?? 1883} onChange={(x) => sMq('port', x)} /></Field>
              <Field label="User"><Input value={mq.username} onInput={(x) => sMq('username', x)} /></Field>
              <Field label="Password"><Input type="password" value={mq.password} onInput={(x) => sMq('password', x)} /></Field>
            </div>
            <ToggleRow title="Encrypt (TLS)" desc="Port 8883 for most brokers and cloud MQTT services" checked={!!mq.tls} onChange={(x) => set({ mqtt: { ...mq, tls: x, port: x && (mq.port ?? 1883) === 1883 ? 8883 : !x && mq.port === 8883 ? 1883 : mq.port } })} />
            {mq.tls && <TlsFields v={mq} set={sMq} />}
            <Field label="Topic" help="{device} = camera id. Events on <topic>/events and <topic>/events/<type>, online state on <topic>/state"><Input value={mq.topic} onInput={(x) => sMq('topic', x)} /></Field>
            <ToggleRow title="Home Assistant discovery" desc="The camera appears as a device with motion, person, last event, last plate and line-counter sensors" checked={!!mq.ha_discovery} onChange={(x) => sMq('ha_discovery', x)} />
            <ToggleRow title="Include snapshot (base64)" checked={!!mq.include_image} onChange={(x) => sMq('include_image', x)} />
          </div>
        </Card>
        <Card title="Telegram" actions={stat('telegram')}>
          <div class="form">
            <ToggleRow title="Send alerts to a Telegram chat" desc="Create a bot with @BotFather, add it to your chat, paste its token and the chat id" checked={!!tg.enabled} onChange={(x) => sTg('enabled', x)} />
            <div class="form g2">
              <Field label="Bot token"><Input type="password" value={tg.token} onInput={(x) => sTg('token', x)} /></Field>
              <Field label="Chat id"><Input value={tg.chat_id} onInput={(x) => sTg('chat_id', x)} placeholder="-1001234567890" /></Field>
              <Field label="At most one message every"><Num value={tg.min_interval_s ?? 5} min={1} unit="s" onChange={(x) => sTg('min_interval_s', x)} /></Field>
            </div>
            <ToggleRow title="Include the snapshot" checked={tg.include_image !== false} onChange={(x) => sTg('include_image', x)} />
            <Filters v={tg} set={sTg} rules={rules} />
          </div>
        </Card>
        <Card title="Email (SMTP)" actions={stat('smtp')}>
          <div class="form">
            <ToggleRow title="Send alerts by email" checked={!!sm.enabled} onChange={(x) => sSm('enabled', x)} />
            <div class="form g2">
              <Field label="Server"><Input value={sm.host} onInput={(x) => sSm('host', x)} placeholder="smtp.example.com" /></Field>
              <Field label="Port"><Num value={sm.port ?? 587} onChange={(x) => sSm('port', x)} /></Field>
              <Field label="Security"><Select value={sm.security || 'starttls'} onChange={(x) => set({ smtp: { ...sm, security: x, port: x === 'tls' ? 465 : x === 'starttls' ? 587 : 25 } })} options={[['starttls', 'STARTTLS (587)'], ['tls', 'SSL/TLS (465)'], ['none', 'None (25)']]} /></Field>
              <Field label="At most one mail every"><Num value={sm.min_interval_s ?? 30} min={5} unit="s" onChange={(x) => sSm('min_interval_s', x)} /></Field>
              <Field label="User"><Input value={sm.user} onInput={(x) => sSm('user', x)} /></Field>
              <Field label="Password"><Input type="password" value={sm.password} onInput={(x) => sSm('password', x)} /></Field>
              <Field label="From"><Input value={sm.from} onInput={(x) => sSm('from', x)} placeholder="camera@example.com" /></Field>
              <Field label="To" help="Comma separated"><Input value={sm.to} onInput={(x) => sSm('to', x)} placeholder="me@example.com" /></Field>
            </div>
            <Field label="Subject"><Input value={sm.subject} onInput={(x) => sSm('subject', x)} /></Field>
            {sm.security !== 'none' && <TlsFields v={sm} set={sSm} />}
            <ToggleRow title="Attach the snapshot" checked={sm.include_image !== false} onChange={(x) => sSm('include_image', x)} />
            <Filters v={sm} set={sSm} rules={rules} />
          </div>
        </Card>
        <Card title="FTP upload" actions={stat('ftp')}>
          <div class="form">
            <ToggleRow title="Upload event pictures to an FTP server" desc="Plain FTP (passive mode): use it on a trusted network. Files go to <path>/<date>/." checked={!!ftp.enabled} onChange={(x) => sFtp('enabled', x)} />
            <div class="form g2">
              <Field label="Server"><Input value={ftp.host} onInput={(x) => sFtp('host', x)} placeholder="192.168.1.20" /></Field>
              <Field label="Port"><Num value={ftp.port ?? 21} onChange={(x) => sFtp('port', x)} /></Field>
              <Field label="User"><Input value={ftp.user} onInput={(x) => sFtp('user', x)} /></Field>
              <Field label="Password"><Input type="password" value={ftp.password} onInput={(x) => sFtp('password', x)} /></Field>
              <Field label="Folder"><Input value={ftp.path} onInput={(x) => sFtp('path', x)} /></Field>
              <Field label="Pictures"><Select value={ftp.what || 'snapshot'} onChange={(x) => sFtp('what', x)} options={[['snapshot', 'Snapshot only'], ['all', 'Snapshot + crops']]} /></Field>
            </div>
            <Filters v={ftp} set={sFtp} rules={rules} />
          </div>
        </Card>
        <Card title="Relay / lamp outputs" hint="Pulse a GPIO pin on events" tight actions={<Btn sm icon="plus" onClick={() => setGedit({ name: '', gpio: -1, pulse_ms: 1000, active_high: true, enabled: true, types: [] })}>Add</Btn>}>
          {!gpios.length && <Empty icon="bolt">No outputs</Empty>}
          <table class="table"><tbody>
            {gpios.map((g, i) => (
              <tr class="click" tabIndex={0} onClick={() => setGedit({ ...g, _i: i })}>
                <td><b>{g.name || `GPIO ${g.gpio}`}</b><div class="dim small">GPIO {g.gpio} · {g.pulse_ms ?? 1000} ms · {(g.rules || []).length ? `${g.rules.length} rule(s)` : (g.types || []).join(', ') || 'all events'}</div></td>
                <td class="right" onClick={(e) => e.stopPropagation()}>{g.enabled === false && <Badge>off</Badge>}
                  <Btn sm kind="ghost" icon="trash" onClick={() => set({ gpio: gpios.filter((_, j) => j !== i) })} /></td>
              </tr>
            ))}
          </tbody></table>
        </Card>
        <Card title="EdgeAI server" actions={stat('edgeai_server')}>
          <div class="form">
            <ToggleRow title="Send events to the EdgeAI server" desc="The server in this project: vehicle/plate and face matching dashboards" checked={!!ea.enabled} onChange={(x) => sEa('enabled', x)} />
            <Field label="Server URL"><Input value={ea.url} onInput={(x) => sEa('url', x)} placeholder="http://192.168.1.10:8090" /></Field>
            {(ea.url || '').startsWith('https://') && <TlsFields v={ea} set={sEa} />}
            <Field label="Heartbeat"><Num value={ea.heartbeat_s ?? 10} min={5} onChange={(x) => sEa('heartbeat_s', x)} unit="s" /></Field>
          </div>
        </Card>
        <Card title="Test">
          <p class="muted small">Sends a test event through every enabled integration.</p>
          <Btn icon="play" onClick={() => api.post('/api/integrations/test').then(() => toast('Test event sent', 'ok'))}>Send test event</Btn>
        </Card>
      </div>
      <SaveBar dirty={dirty} onSave={() => save()} onReset={reload} />
      {edit && <WebhookEdit w={edit} rules={rules} onClose={() => setEdit(null)} onSave={(w) => {
        const { _i, ...clean } = w;
        set({ webhooks: _i === undefined ? [...hooks, clean] : hooks.map((x, j) => (j === _i ? clean : x)) });
        setEdit(null);
      }} />}
      {gedit && <GpioEdit g={gedit} rules={rules} onClose={() => setGedit(null)} onSave={(g) => {
        const { _i, ...clean } = g;
        set({ gpio: _i === undefined ? [...gpios, clean] : gpios.map((x, j) => (j === _i ? clean : x)) });
        setGedit(null);
      }} />}
    </div>
  );
}

function Api() {
  const [keys, setKeys] = useState<any[]>([]);
  const [name, setName] = useState('');
  const [role, setRole] = useState('viewer');
  const [created, setCreated] = useState('');
  const load = () => isAdmin() && api.get('/api/keys').then(setKeys, () => {});
  useEffect(() => {
    load();
  }, []);
  const base = `http://${location.host}`;
  return (
    <div class="grid g2">
      <Card title="API keys" hint="For scripts and third-party systems">
        {isAdmin() ? (
          <div class="col">
            <div class="row">
              <div class="grow"><Input value={name} onInput={setName} placeholder="Key name, e.g. parking-system" /></div>
              <div style={{ width: 130 }}><Select value={role} onChange={setRole} options={[['viewer', 'Viewer'], ['operator', 'Operator'], ['admin', 'Admin']]} /></div>
              <Btn kind="primary" icon="key" disabled={!name} onClick={async () => {
                try {
                  const r = await api.post('/api/keys', { name, role });
                  setCreated(r.key);
                  setKeys(r.keys);
                  setName('');
                } catch (e: any) {
                  toast(e.message, 'err');
                }
              }}>Create</Btn>
            </div>
            {created && <Callout kind="ok"><div>Copy this key now, it is not shown again:<div class="mt"><Copy text={created} /></div></div></Callout>}
            <table class="table"><tbody>
              {keys.map((k) => (
                <tr><td><b>{k.name}</b><div class="mono dim small">{k.prefix}…</div></td><td><Badge>{k.role}</Badge></td>
                  <td class="right"><Btn sm kind="ghost" icon="trash" onClick={async () => {
                    if (await confirmDialog('Revoke key', `Revoke “${k.name}”? Systems using it lose access.`, 'Revoke', true)) setKeys(await api.del(`/api/keys/${encodeURIComponent(k.name)}`));
                  }} /></td></tr>
              ))}
            </tbody></table>
            {!keys.length && <div class="dim small">No keys</div>}
          </div>
        ) : <div class="dim">Admins can create API keys.</div>}
      </Card>
      <Card title="HTTP API">
        <div class="col small">
          <div class="muted">Authenticate with <code>Authorization: Bearer pk_…</code> (or <code>?token=pk_…</code>), HTTP Basic/Digest also works.</div>
          <Copy text={`${base}/api/snapshot.jpg`} />
          <Copy text={`${base}/api/events?limit=20&type=track`} />
          <Copy text={`${base}/api/events/export.csv?date=${new Date().toISOString().slice(0, 10)}`} />
          <Copy text={`${base}/api/status`} />
          <Copy text={`ws://${location.host}/ws/events?token=pk_…`} />
          <div class="dim">WebSocket topics: <code>event</code>, <code>detections</code>, <code>motion</code>, <code>status</code>.</div>
        </div>
      </Card>
    </div>
  );
}

export function IntegrationsPage() {
  const [tab, setTab] = useState('streaming');
  return (
    <div>
      <Tabs value={tab} onChange={setTab} tabs={[['streaming', 'Streaming & ONVIF'], ['events', 'Event delivery'], ['api', 'API & keys']]} />
      {tab === 'streaming' && <Streaming />}
      {tab === 'events' && <Events />}
      {tab === 'api' && <Api />}
    </div>
  );
}
