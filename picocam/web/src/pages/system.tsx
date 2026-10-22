import { useEffect, useState } from 'preact/hooks';
import { api, fmtDuration, uploadWithProgress } from '../api';
import { isAdmin, session, status, useStore } from '../store';
import { Icon } from '../ui/icons';
import { Badge, Btn, Callout, Card, confirmDialog, Field, Input, Modal, Select, Seg, Stat, Tabs, toast, ToggleRow } from '../ui/kit';
import { AuditTab, SecurityTab } from './security';

const MM_NOTE: Record<number, string> = { 24: 'standard', 28: 'ANPR + 1080p', 32: 'ANPR + 3 MP' };
const MM_FITS: Record<number, string> = { 24: '3 MP video + one small detector (person/vehicle)', 28: '3 MP + detector, or 1080p + full ANPR (3 models)', 32: '3 MP + full ANPR / face pipelines' };

function Overview() {
  const st = useStore(status);
  const [info, setInfo] = useState<any>(null);
  useEffect(() => {
    api.get('/api/status').then(setInfo, () => {});
  }, []);
  const s = st?.system || info?.system;
  const d = info?.device;
  return (
    <div class="col">
      {s && (
        <div class="grid g4">
          <Stat icon="cpu" k="CPU" v={s.cpu_pct} unit="%" pct={s.cpu_pct} />
          <Stat icon="memory" k="Free memory" v={Math.round(s.mem_avail_kb / 1024)} unit="MB" pct={100 - (s.mem_avail_kb / s.mem_total_kb) * 100} />
          <Stat icon="layers" k="Media memory" v={Math.round(s.media_mem_used_kb / 1024)} unit={`/ ${Math.round(s.media_mem_total_kb / 1024)} MB`} pct={(s.media_mem_used_kb / s.media_mem_total_kb) * 100} />
          <Stat icon="clock" k="Uptime" v={fmtDuration(s.uptime_s)} />
        </div>
      )}
      {d && (
        <Card title="Device information">
          <dl class="kv">
            <dt>Model</dt><dd>{d.model}</dd>
            <dt>SoC</dt><dd>{d.soc}</dd>
            <dt>Sensor</dt><dd>{d.sensor}</dd>
            {d.board && <><dt>Memory</dt><dd>{d.board.ram_mb} MB</dd><dt>Boot from</dt><dd>{({ 'spi-nand': 'SPI NAND flash', emmc: 'eMMC', sd: 'SD card' } as any)[d.board.boot] || 'unknown'}{d.board.flash_limited && <> <Badge>protected: low write wear</Badge></>}</dd></>}
            <dt>Firmware</dt><dd>PicoCam {d.firmware}</dd>
            <dt>Kernel</dt><dd>{d.kernel}</dd>
            <dt>Serial / ID</dt><dd class="mono">{d.id}</dd>
            <dt>MAC</dt><dd class="mono">{d.mac}</dd>
          </dl>
        </Card>
      )}
    </div>
  );
}

function Performance() {
  const [mm, setMm] = useState<any>(null);
  const [pick, setPick] = useState('');
  useEffect(() => {
    api.get('/api/system/media-memory').then((m) => {
      setMm(m);
      setPick(String(m.configured_mb));
    }, () => {});
  }, []);
  if (!mm) return <div class="empty"><span class="spin" /></div>;
  const ram = Math.round(mm.total_ram_kb / 1024);
  return (
    <div class="grid g2">
      <Card title="Media memory" hint="Shared by the ISP, video encoders and NPU (AI models)">
        <div class="form">
          <dl class="kv">
            <dt>Active</dt><dd>{mm.active_mb} MB ({Math.round(mm.used_kb / 1024)} MB in use)</dd>
            <dt>After reboot</dt><dd>{mm.configured_mb} MB</dd>
            <dt>System RAM now</dt><dd>{ram} MB</dd>
          </dl>
          <Field label="Size" help="More media memory = bigger AI pipelines with full-resolution video, but less RAM for the system">
            <Seg value={pick} onChange={setPick} options={(mm.options || [24, 28, 32]).map((o: number) => [String(o), `${o} MB${MM_NOTE[o] ? ' · ' + MM_NOTE[o] : ''}`])} disabled={!mm.changeable || !isAdmin()} />
          </Field>
          <table class="table small">
            <thead><tr><th>Size</th><th>Fits</th><th>System RAM</th></tr></thead>
            <tbody>
              {(mm.options || [24, 28, 32]).map((o: number) => (
                <tr key={o}><td>{o} MB</td><td>{MM_FITS[o] || 'More headroom for several models at full resolution'}</td><td>~{ram + mm.active_mb - o} MB</td></tr>
              ))}
            </tbody>
          </table>
          {!mm.changeable && <Callout kind="warn">{mm.note}</Callout>}
          {mm.changeable && isAdmin() && (
            <Btn kind="primary" disabled={pick === String(mm.configured_mb)} onClick={async () => {
              if (!(await confirmDialog('Change media memory', `Set media memory to ${pick} MB and reboot now? The boot settings are verified and a backup is kept.`, 'Apply & reboot'))) return;
              try {
                await api.post('/api/system/media-memory', { mb: +pick, reboot: true });
                toast('Rebooting… reload this page in about a minute', 'ok');
              } catch (e: any) {
                toast(e.message, 'err');
              }
            }}>Apply & reboot</Btn>
          )}
        </div>
      </Card>
      <Card title="Tips">
        <div class="col small muted">
          <div>• The sub stream feeds the AI. 960×540 reads plates up to ~6 m with the stock lens; raise it for longer range.</div>
          <div>• H.265 halves bandwidth and SD usage for the same quality.</div>
          <div>• SmartP encoding cuts bitrate a lot for static scenes (parking, warehouses).</div>
          <div>• For the lowest live delay use the web live view (WebCodecs/MSE) or VLC with 150 ms network caching.</div>
        </div>
      </Card>
    </div>
  );
}

function Users() {
  const me = useStore(session)!;
  const [users, setUsers] = useState<any[]>([]);
  const [edit, setEdit] = useState<any>(null);
  const [pw, setPw] = useState({ current: '', password: '', confirm: '' });
  useEffect(() => {
    if (isAdmin()) api.get('/api/users').then(setUsers, () => {});
  }, []);
  return (
    <div class="grid g2">
      {isAdmin() && (
        <Card title="Users" tight actions={<Btn sm icon="plus" onClick={() => setEdit({ name: '', role: 'viewer', password: '', isNew: true })}>Add user</Btn>}>
          <table class="table"><tbody>
            {users.map((u) => (
              <tr class="click" onClick={() => setEdit({ ...u, password: '' })}>
                <td><b>{u.name}</b> {u.name === me.user && <span class="dim small">(you)</span>}</td>
                <td><Badge kind={u.role === 'admin' ? 'accent' : ''}>{u.role}</Badge></td>
                <td class="right" onClick={(e) => e.stopPropagation()}>
                  {u.name !== me.user && <Btn sm kind="ghost" icon="trash" onClick={async () => {
                    if (!(await confirmDialog('Delete user', `Delete ${u.name}?`, 'Delete', true))) return;
                    api.del(`/api/users/${u.name}`).then(setUsers, (e) => toast(e.message, 'err'));
                  }} />}
                </td>
              </tr>
            ))}
          </tbody></table>
          <div class="small dim" style={{ padding: '10px 14px' }}>Viewer: live + playback. Operator: also settings and AI. Admin: everything. The same accounts work for RTSP and ONVIF.</div>
        </Card>
      )}
      <Card title="Change my password">
        <div class="form">
          <Field label="Current password"><Input type="password" value={pw.current} onInput={(v) => setPw({ ...pw, current: v })} /></Field>
          <Field label="New password" help="8+ characters, letters and digits"><Input type="password" value={pw.password} onInput={(v) => setPw({ ...pw, password: v })} /></Field>
          <Field label="Confirm"><Input type="password" value={pw.confirm} onInput={(v) => setPw({ ...pw, confirm: v })} /></Field>
          <Btn kind="primary" disabled={!pw.password || pw.password !== pw.confirm} onClick={() => api.post('/api/password', pw).then(() => {
            toast('Password changed', 'ok');
            setPw({ current: '', password: '', confirm: '' });
          }, (e) => toast(e.message, 'err'))}>Change password</Btn>
        </div>
      </Card>
      {edit && (
        <Modal title={edit.isNew ? 'Add user' : `Edit ${edit.name}`} onClose={() => setEdit(null)} footer={
          <><Btn kind="ghost" onClick={() => setEdit(null)}>Cancel</Btn>
            <Btn kind="primary" onClick={() => api.post('/api/users', edit).then((u) => {
              setUsers(u);
              setEdit(null);
              toast('Saved', 'ok');
            }, (e) => toast(e.message, 'err'))}>Save</Btn></>
        }>
          <div class="form">
            <Field label="User name"><Input value={edit.name} disabled={!edit.isNew} onInput={(v) => setEdit({ ...edit, name: v })} /></Field>
            <Field label="Role"><Select value={edit.role} onChange={(v) => setEdit({ ...edit, role: v })} options={[['viewer', 'Viewer'], ['operator', 'Operator'], ['admin', 'Admin']]} /></Field>
            <Field label={edit.isNew ? 'Password' : 'New password'} help={edit.isNew ? '' : 'Leave empty to keep the current password'}><Input type="password" value={edit.password} onInput={(v) => setEdit({ ...edit, password: v })} /></Field>
          </div>
        </Modal>
      )}
    </div>
  );
}

function RootMode() {
  const [rm, setRm] = useState<any>(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    api.get('/api/system/root-mode').then(setRm, () => {});
  }, []);
  if (!rm) return null;
  return (
    <Card title="Power-cut protection" hint="Read-only system partition">
      <div class="form">
        <dl class="kv">
          <dt>System partition</dt><dd>{rm.read_only ? <Badge kind="ok">read-only</Badge> : <Badge kind="warn">writable</Badge>}</dd>
        </dl>
        <ToggleRow title="Keep the system partition read-only" desc="Nothing writes to the operating system, so a power cut cannot damage it. Settings, recordings and updates are stored elsewhere and keep working." checked={rm.wanted && rm.supported} disabled={!rm.supported || !isAdmin() || busy} onChange={async (x) => {
          setBusy(true);
          try {
            setRm(await api.post('/api/system/root-mode', { read_only: x }));
            toast(x ? 'System partition is read-only' : 'System partition is writable', 'ok');
          } catch (e: any) {
            toast(e.message, 'err');
          }
          setBusy(false);
        }} />
        {!rm.supported && <Callout kind="warn">Not available: {rm.reason}.</Callout>}
        {rm.supported && rm.wanted && !rm.read_only && <Callout kind="warn">Something still has a file open on the system partition; it becomes read-only on the next restart.</Callout>}
      </div>
    </Card>
  );
}

function Maintenance() {
  const [pct, setPct] = useState(-1);
  const [us, setUs] = useState<any>(null);
  useEffect(() => {
    api.get('/api/system/update-state').then(setUs, () => {});
  }, []);
  const [keepNet, setKeepNet] = useState(true);
  const [wipeUsers, setWipeUsers] = useState(false);
  const update = async (f: File) => {
    if (!(await confirmDialog('Install update', `Install ${f.name}? The camera service restarts (about 20 s).`, 'Install'))) return;
    try {
      setPct(0);
      const r = await uploadWithProgress('/api/system/update', f, setPct);
      toast(`Installed ${r.version}, restarting…`, 'ok');
      setTimeout(() => location.reload(), 20000);
    } catch (e: any) {
      toast(e.message, 'err');
      setPct(-1);
    }
  };
  const restore = async (f: File) => {
    try {
      const r = await api.post('/api/system/restore', JSON.parse(await f.text()));
      toast(r.ok ? 'Configuration restored' : 'Restored with errors: ' + JSON.stringify(r.errors), r.ok ? 'ok' : 'err');
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  return (
    <div class="grid g2">
      <Card title="Firmware update">
        {us?.rolled_back && <Callout kind="warn">The last update kept crashing, so the camera went back to the previous version automatically.</Callout>}
        {us?.config_recovered && <Callout kind="warn">The camera kept crashing with the last settings and restored the last working configuration.</Callout>}
        <dl class="kv" style={{ marginBottom: 12 }}>
          <dt>Running</dt><dd>PicoCam {us?.version || '…'} {us?.pending && <Badge kind="warn">on probation: confirmed after 2 stable minutes</Badge>}</dd>
          <dt>Previous</dt><dd>{us?.previous_version ? `PicoCam ${us.previous_version}` : <span class="dim">none stored</span>}</dd>
        </dl>
        {us?.previous_version && (
          <div style={{ marginBottom: 12 }}>
            <Btn icon="undo" onClick={async () => {
              if (!(await confirmDialog('Roll back', `Go back to PicoCam ${us.previous_version}? The camera service restarts.`, 'Roll back', true))) return;
              await api.post('/api/system/rollback').then(() => (toast('Rolling back, restarting…', 'ok'), setTimeout(() => location.reload(), 15000)), (e) => toast(e.message, 'err'));
            }}>Roll back to {us.previous_version}</Btn>
          </div>
        )}
        <label class="drop">
          <input type="file" accept=".tar.gz,.tgz" hidden onChange={(e) => {
            const f = (e.target as HTMLInputElement).files?.[0];
            if (f) update(f);
          }} />
          <Icon n="upload" s={26} />
          <div>{pct >= 0 ? `Uploading… ${pct}%` : 'Choose a picocam-x.y.z.tar.gz package'}</div>
          <div class="small dim">Settings, users, models and recordings are kept. The package is checked before it is installed; if the new version keeps crashing the camera goes back by itself.</div>
        </label>
      </Card>
      <Card title="Configuration backup">
        <div class="col">
          <a class="btn" href="/api/system/backup" download="picocam-config.json"><Icon n="download" s={16} /> Download backup</a>
          <label class="btn"><Icon n="upload" s={16} /> Restore from file
            <input type="file" accept=".json" hidden onChange={(e) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (f) restore(f);
            }} />
          </label>
          <div class="small dim">Passwords are not included in backups.</div>
          <a class="btn" href="/api/system/diagnostics" download="picocam-diagnostics.json"><Icon n="download" s={16} /> Download diagnostics</a>
          <div class="small dim">Versions, status, settings (without passwords), logs and crash reports in one file for support.</div>
        </div>
      </Card>
      <RootMode />
      <Card title="Restart">
        <div class="row">
          <Btn icon="refresh" onClick={async () => (await confirmDialog('Restart service', 'Restart the camera service (about 10 s)?')) && api.post('/api/system/restart').then(() => toast('Restarting…'))}>Restart service</Btn>
          <Btn icon="power" kind="danger" onClick={async () => (await confirmDialog('Reboot', 'Reboot the camera (about a minute)?', 'Reboot', true)) && api.post('/api/system/reboot').then(() => toast('Rebooting…'))}>Reboot</Btn>
        </div>
      </Card>
      <Card title="Factory reset">
        <div class="form">
          <ToggleRow title="Keep network settings" checked={keepNet} onChange={setKeepNet} />
          <ToggleRow title="Also delete users (re-activation needed)" checked={wipeUsers} onChange={setWipeUsers} />
          <Btn kind="danger solid" onClick={async () => {
            if (!(await confirmDialog('Factory reset', 'All settings return to defaults and the camera reboots.', 'Reset', true))) return;
            api.post('/api/system/factory-reset', { keep_network: keepNet, users: wipeUsers }).then(() => toast('Resetting…'));
          }}>Reset to factory defaults</Btn>
        </div>
      </Card>
    </div>
  );
}

function Logs() {
  const [logs, setLogs] = useState<any[]>([]);
  const [lvl, setLvl] = useState('info');
  const load = () => api.get('/api/system/logs?n=400').then(setLogs, () => {});
  useEffect(() => {
    load();
    const t = setInterval(load, 4000);
    return () => clearInterval(t);
  }, []);
  const rank: any = { debug: 0, info: 1, warn: 2, error: 3 };
  const color: any = { warn: 'var(--warn)', error: 'var(--bad)', info: 'var(--text-2)', debug: 'var(--text-3)' };
  return (
    <Card title="System log" actions={<Seg value={lvl} onChange={setLvl} options={[['info', 'Info'], ['warn', 'Warnings'], ['error', 'Errors']]} />} tight>
      <div style={{ maxHeight: 600, overflow: 'auto', fontFamily: 'var(--mono)', fontSize: 12, padding: '8px 14px' }}>
        {[...logs].reverse().filter((l) => rank[l.level] >= rank[lvl]).map((l) => (
          <div style={{ color: color[l.level], padding: '2px 0' }}>
            <span class="dim">{new Date(l.ts).toLocaleTimeString()}</span> <b>{l.tag}</b> {l.msg}
          </div>
        ))}
      </div>
    </Card>
  );
}

export function SystemPage() {
  const [tab, setTab] = useState('overview');
  const tabs: [string, string][] = [['overview', 'Overview'], ['performance', 'Performance'], ['users', 'Users'], ['logs', 'Logs']];
  if (isAdmin()) tabs.splice(2, 0, ['security', 'Security']), tabs.splice(4, 0, ['audit', 'Audit log']), tabs.splice(5, 0, ['maintenance', 'Maintenance']);
  return (
    <div>
      <Tabs value={tab} onChange={setTab} tabs={tabs} />
      {tab === 'overview' && <Overview />}
      {tab === 'performance' && <Performance />}
      {tab === 'users' && <Users />}
      {tab === 'maintenance' && <Maintenance />}
      {tab === 'security' && <SecurityTab />}
      {tab === 'audit' && <AuditTab />}
      {tab === 'logs' && <Logs />}
    </div>
  );
}
