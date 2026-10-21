import { useEffect, useState } from 'preact/hooks';
import { api, fmtTime } from '../api';
import { session, useStore } from '../store';
import { Badge, Btn, Callout, Card, confirmDialog, Field, Input, Modal, Num, SaveBar, toast, ToggleRow, upd, useSection } from '../ui/kit';

// System > Security: remote shells, watchdog, who may connect, HTTPS, signed-in sessions
export function SecurityTab() {
  const sys = useSection<any>('system');
  const sec = useSection<any>('security');
  const web = useSection<any>('web');
  const me = useStore(session);
  const [st, setSt] = useState<any>(null);
  const [tls, setTls] = useState<any>(null);
  const [sessions, setSessions] = useState<any[]>([]);
  const [upload, setUpload] = useState(false);
  const load = () => {
    api.get('/api/system/security').then(setSt, () => {});
    api.get('/api/system/tls').then(setTls, () => {});
    api.get('/api/sessions').then(setSessions, () => {});
  };
  useEffect(load, []);
  const s = sys.val, w = web.val, c = sec.val;
  if (!s || !w || !c) return <div class="empty"><span class="spin" /></div>;
  const https = w.https || {};
  const allow: string[] = c.allow || [];
  const dirty = sys.dirty || sec.dirty || web.dirty;
  const saveAll = async () => {
    for (const x of [sys, sec, web]) if (x.dirty && !(await x.save())) return;
    load();
  };
  const resetAll = () => {
    sys.reload();
    sec.reload();
    web.reload();
  };
  return (
    <div class="col">
      {st && st.ssh && st.root_synced !== 1 && <Callout kind="warn">The shell (SSH) is on and its root password has not been tied to the admin password. Turn on “Root password follows admin password” and set the admin password again, or turn SSH off.</Callout>}
      <div class="grid g2">
        <Card title="Remote access" hint="Shell access to the board itself">
          <div class="form">
            <ToggleRow title="SSH" desc="Command-line access as root. Turn off when you do not need it." checked={s.ssh !== false} onChange={(x) => sys.set({ ssh: x })} />
            <ToggleRow title="ADB (USB debugging)" desc="Android-style debug shell over the USB cable" checked={s.adb !== false} onChange={(x) => sys.set({ adb: x })} />
            <ToggleRow title="Root password follows the admin password" desc="One password for the portal and the shell: changing the admin password also changes root’s"
              checked={s.root_sync !== false} onChange={(x) => sys.set({ root_sync: x })} />
            {(s.ssh === false && s.adb === false) && <Callout kind="warn">With SSH and ADB both off the only way in is this portal. If it ever breaks, a factory reset from the board is the way back.</Callout>}
          </div>
        </Card>
        <Card title="Self-healing">
          <div class="form">
            <ToggleRow title="Hardware watchdog" desc="Resets the board if the camera program or the video encoder hangs"
              checked={s.watchdog !== false} onChange={(x) => sys.set({ watchdog: x })} />
            <dl class="kv">
              <dt>Watchdog</dt><dd>{st?.watchdog_armed ? <Badge kind="ok">armed</Badge> : <Badge>{st?.watchdog ? 'not available on this board' : 'off'}</Badge>}</dd>
              <dt>SSH service</dt><dd>{st?.ssh_running ? <Badge kind="warn">running</Badge> : <Badge kind="ok">stopped</Badge>}</dd>
            </dl>
          </div>
        </Card>
        <Card title="Who may connect" hint="Applies to the portal, RTSP and ONVIF">
          <div class="form">
            <Field label="Allowed addresses" help="One per line, e.g. 192.168.1.0/24 or 10.0.0.5. Empty = everyone on the network. You cannot lock yourself out: the list must contain your own address.">
              <textarea class="input mono" value={allow.join('\n')} placeholder="192.168.1.0/24"
                onInput={(e) => sec.set({ allow: (e.target as HTMLTextAreaElement).value.split(/[\s,]+/).map((x) => x.trim()).filter(Boolean) })} />
            </Field>
            {me?.ip && <div class="small dim">Your address: <span class="mono">{me.ip}</span> {!allow.includes(me.ip) && <a onClick={() => sec.set({ allow: [...allow, me.ip] })}>add</a>}</div>}
          </div>
        </Card>
        <Card title="Web & HTTPS">
          <div class="form">
            <div class="form g2">
              <Field label="HTTP port"><Num value={w.port} min={1} max={65535} onChange={(x) => web.set({ port: x })} /></Field>
              <Field label="HTTPS port"><Num value={https.port ?? 443} min={1} max={65535} onChange={(x) => web.set(upd(w, 'https.port', x))} disabled={https.enabled === false} /></Field>
            </div>
            <ToggleRow title="HTTPS" desc="Encrypted portal and WebSocket. Also enables the lowest-delay live player." checked={https.enabled !== false} onChange={(x) => web.set(upd(w, 'https.enabled', x))} />
            <ToggleRow title="Send browsers to HTTPS" desc="Plain-HTTP page requests are redirected. NVR, ONVIF and snapshot URLs stay on HTTP." checked={!!https.redirect} disabled={https.enabled === false} onChange={(x) => web.set(upd(w, 'https.redirect', x))} />
            <Field label="Session length"><Num value={w.session_hours} min={1} max={720} unit="hours" onChange={(x) => web.set({ session_hours: x })} /></Field>
          </div>
        </Card>
      </div>
      <SaveBar dirty={dirty} onSave={saveAll} onReset={resetAll} note="Port and access changes apply immediately; keep this page open to confirm they work" />
      <div class="grid g2">
        <Card title="HTTPS certificate" actions={<Btn sm icon="refresh" onClick={async () => {
          if (!(await confirmDialog('New certificate', 'Create a new self-signed certificate? Browsers will ask you to trust it again.', 'Create'))) return;
          try {
            setTls({ ...(await api.post('/api/system/tls/regenerate')), enabled: https.enabled });
            toast('New certificate created', 'ok');
          } catch (e: any) {
            toast(e.message, 'err');
          }
        }}>New self-signed</Btn>}>
          {tls?.available ? (
            <div class="col">
              <dl class="kv">
                <dt>Subject</dt><dd class="mono">{tls.subject}</dd>
                <dt>Valid until</dt><dd>{tls.not_after}</dd>
                <dt>Type</dt><dd>{tls.self_signed ? <Badge kind="warn">self-signed (browsers warn once)</Badge> : <Badge kind="ok">issued by a CA</Badge>}</dd>
                <dt>SHA-256</dt><dd class="mono small" style={{ wordBreak: 'break-all' }}>{tls.fingerprint}</dd>
              </dl>
              <div class="row"><Btn sm icon="upload" onClick={() => setUpload(true)}>Use my own certificate</Btn></div>
            </div>
          ) : <div class="dim">No certificate: HTTPS is unavailable.</div>}
        </Card>
        <Card title="Signed-in sessions" actions={sessions.length > 1 && <Btn sm onClick={async () => setSessions(await api.del('/api/sessions/others'))}>Sign out all others</Btn>} tight>
          <table class="table"><tbody>
            {sessions.map((x) => (
              <tr>
                <td><b>{x.user}</b> <span class="dim small">{x.role}</span> {x.current && <Badge kind="ok">this browser</Badge>}
                  <div class="dim small">{x.peer} · since {fmtTime(x.created)}</div></td>
                <td class="right">{!x.current && <Btn sm kind="ghost" icon="x" onClick={async () => setSessions(await api.del(`/api/sessions/${x.id}`))} />}</td>
              </tr>
            ))}
          </tbody></table>
          {!sessions.length && <div class="dim small" style={{ padding: 14 }}>No sessions</div>}
        </Card>
      </div>
      {upload && <UploadCert onClose={() => setUpload(false)} onDone={(i) => { setTls({ ...i, enabled: https.enabled }); setUpload(false); }} />}
    </div>
  );
}

function UploadCert(p: { onClose: () => void; onDone: (i: any) => void }) {
  const [cert, setCert] = useState('');
  const [key, setKey] = useState('');
  const file = (set: (v: string) => void) => (e: Event) => {
    const f = (e.target as HTMLInputElement).files?.[0];
    if (f) f.text().then(set);
  };
  return (
    <Modal title="Use my own certificate" onClose={p.onClose} footer={<><Btn kind="ghost" onClick={p.onClose}>Cancel</Btn>
      <Btn kind="primary" icon="check" disabled={!cert || !key} onClick={async () => {
        try {
          p.onDone(await api.post('/api/system/tls/upload', { cert, key }));
          toast('Certificate installed', 'ok');
        } catch (e: any) {
          toast(e.message, 'err');
        }
      }}>Install</Btn></>}>
      <div class="form">
        <Callout>PEM files. Include the intermediate certificates after your own in the certificate file. The private key stays on the camera.</Callout>
        <Field label="Certificate (PEM)"><textarea class="input mono" value={cert} onInput={(e) => setCert((e.target as HTMLTextAreaElement).value)} placeholder="-----BEGIN CERTIFICATE-----" /><input type="file" accept=".pem,.crt,.cer" onChange={file(setCert)} /></Field>
        <Field label="Private key (PEM)"><textarea class="input mono" value={key} onInput={(e) => setKey((e.target as HTMLTextAreaElement).value)} placeholder="-----BEGIN PRIVATE KEY-----" /><input type="file" accept=".pem,.key" onChange={file(setKey)} /></Field>
      </div>
    </Modal>
  );
}

const ACTION_LABEL: Record<string, string> = {
  login: 'Signed in', login_failed: 'Failed sign-in', config: 'Changed settings', user_set: 'User saved', user_delete: 'User deleted',
  password_change: 'Password changed', key_create: 'API key created', key_delete: 'API key revoked', reboot: 'Reboot', restart_service: 'Service restart',
  factory_reset: 'Factory reset', firmware_update: 'Firmware update', firmware_rollback: 'Firmware rolled back', media_memory: 'Media memory',
  activate: 'Camera activated', network_confirm: 'Network change confirmed', session_revoked: 'Session ended', sessions_revoked: 'Sessions ended',
  tls_regenerate: 'New certificate', tls_upload: 'Certificate installed', plugin_install: 'Plugin installed', plugin_remove: 'Plugin removed',
  face_delete: 'Face removed',
};

export function AuditTab() {
  const [rows, setRows] = useState<any[]>([]);
  const [q, setQ] = useState('');
  useEffect(() => {
    api.get('/api/system/audit?n=300').then(setRows, () => {});
  }, []);
  const f = rows.filter((r) => !q || JSON.stringify(r).toLowerCase().includes(q.toLowerCase()));
  return (
    <div class="col">
      <div class="card" style={{ padding: 12 }}>
        <div class="row">
          <div style={{ flex: '1 1 240px' }}><Input value={q} onInput={setQ} placeholder="Filter by user, address or action…" /></div>
          <a class="btn" href="/api/system/diagnostics" download="picocam-diagnostics.json"><span>Download diagnostics</span></a>
        </div>
      </div>
      <Card title="Security log" hint="Sign-ins, setting changes and administrative actions" tight>
        <table class="table small">
          <thead><tr><th>Time</th><th>User</th><th>Address</th><th>Action</th><th>Details</th></tr></thead>
          <tbody>
            {f.map((r) => (
              <tr>
                <td class="nowrap">{fmtTime(r.ts)}</td><td>{r.user}</td><td class="mono">{r.peer}</td>
                <td>{r.action === 'login_failed' ? <Badge kind="bad">{ACTION_LABEL[r.action]}</Badge> : ACTION_LABEL[r.action] || r.action}</td>
                <td class="muted">{r.detail}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {!f.length && <div class="dim small" style={{ padding: 14 }}>No entries</div>}
      </Card>
    </div>
  );
}
