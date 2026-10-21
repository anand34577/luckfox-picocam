import { useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { Badge, Btn, Callout, Card, Field, Input, Num, SaveBar, Seg, Select, toast, ToggleRow, useSection } from '../ui/kit';

// POSIX TZ strings (sign is inverted: IST-5:30 is UTC+5:30)
export const ZONES: [string, string, string][] = [
  ['Asia/Kolkata', 'IST-5:30', 'India (UTC+5:30)'],
  ['UTC', 'UTC0', 'UTC'],
  ['Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0', 'London'],
  ['Europe/Berlin', 'CET-1CEST,M3.5.0,M10.5.0/3', 'Central Europe'],
  ['Asia/Dubai', '<+04>-4', 'Dubai (UTC+4)'],
  ['Asia/Karachi', 'PKT-5', 'Pakistan (UTC+5)'],
  ['Asia/Dhaka', '<+06>-6', 'Bangladesh (UTC+6)'],
  ['Asia/Kathmandu', '<+0545>-5:45', 'Nepal (UTC+5:45)'],
  ['Asia/Bangkok', '<+07>-7', 'Bangkok (UTC+7)'],
  ['Asia/Shanghai', 'CST-8', 'China (UTC+8)'],
  ['Asia/Singapore', '<+08>-8', 'Singapore (UTC+8)'],
  ['Asia/Tokyo', 'JST-9', 'Japan (UTC+9)'],
  ['Australia/Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3', 'Sydney'],
  ['America/New_York', 'EST5EDT,M3.2.0,M11.1.0', 'US Eastern'],
  ['America/Chicago', 'CST6CDT,M3.2.0,M11.1.0', 'US Central'],
  ['America/Los_Angeles', 'PST8PDT,M3.2.0,M11.1.0', 'US Pacific'],
  ['America/Sao_Paulo', '<-03>3', 'São Paulo (UTC-3)'],
];

export function NetworkPage() {
  const net = useSection<any>('network');
  const time = useSection<any>('time');
  const dev = useSection<any>('device');
  const [ns, setNs] = useState<any>(null);
  const [clock, setClock] = useState<number>(0);
  useEffect(() => {
    api.get('/api/status').then((s) => {
      setNs(s.network);
      setClock(s.system.time_ms - Date.now());
    }, () => {});
  }, []);
  const n = net.val, t = time.val, d = dev.val;
  if (!n || !t || !d) return <div class="empty"><span class="spin" /></div>;
  const hasEth = ns?.interfaces?.some((i: any) => i.name === 'eth0');
  return (
    <div class="col">
      <div class="grid g2">
        <Card title="Device">
          <div class="form">
            <Field label="Camera name" help="Shown in the OSD, ONVIF and integrations"><Input value={d.name} onInput={(x) => dev.set({ name: x })} /></Field>
            <Field label="Location"><Input value={d.location} onInput={(x) => dev.set({ location: x })} placeholder="e.g. Main gate" /></Field>
            <Field label="Device ID"><Input value={d.id} onInput={() => {}} disabled /></Field>
          </div>
          <SaveBar dirty={dev.dirty} onSave={() => dev.save()} onReset={dev.reload} />
        </Card>
        <Card title="Interfaces">
          <table class="table">
            <tbody>
              {ns?.interfaces?.map((i: any) => (
                <tr>
                  <td><b>{i.name}</b> <span class="dim small">{i.name === 'usb0' ? 'USB' : i.name === 'eth0' ? 'Ethernet' : ''}</span></td>
                  <td class="mono">{i.ip || '–'}<div class="dim">{i.mac}</div></td>
                  <td class="right">{i.link ? <Badge kind="ok">up{i.speed_mbps > 0 ? ` ${i.speed_mbps} Mb/s` : ''}</Badge> : <Badge>down</Badge>}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <dl class="kv mt">
            <dt>Gateway</dt><dd>{ns?.gateway || '–'}</dd>
            <dt>DNS</dt><dd>{ns?.dns?.join(', ') || '–'}</dd>
            <dt>Hostname</dt><dd>{ns?.hostname}</dd>
          </dl>
        </Card>
      </div>
      <div class="grid g2">
        <Card title="IP settings" hint={hasEth ? 'Ethernet (eth0)' : 'This board has no Ethernet: use the USB link below'}>
          <div class="form">
            <Field label="Address"><Seg value={n.mode} onChange={(x) => net.set({ mode: x })} options={[['dhcp', 'DHCP'], ['static', 'Static']]} /></Field>
            {n.mode === 'static' && (
              <div class="form g2">
                <Field label="IP address"><Input value={n.ip} onInput={(x) => net.set({ ip: x })} placeholder="192.168.1.64" /></Field>
                <Field label="Netmask"><Input value={n.netmask} onInput={(x) => net.set({ netmask: x })} /></Field>
                <Field label="Gateway"><Input value={n.gateway} onInput={(x) => net.set({ gateway: x })} /></Field>
              </div>
            )}
            <div class="form g2">
              <Field label="DNS 1"><Input value={n.dns?.[0] || ''} onInput={(x) => net.set({ dns: [x, n.dns?.[1] || ''] })} /></Field>
              <Field label="DNS 2"><Input value={n.dns?.[1] || ''} onInput={(x) => net.set({ dns: [n.dns?.[0] || '', x] })} /></Field>
            </div>
            <Field label="Hostname"><Input value={n.hostname} onInput={(x) => net.set({ hostname: x })} /></Field>
            {n.mode === 'static' && <Callout kind="warn">After saving, open the camera at the new address within 60 seconds and press “Keep new settings”, or the old settings come back by themselves.</Callout>}
          </div>
          <SaveBar dirty={net.dirty} onSave={() => net.save()} onReset={net.reload} />
        </Card>
        <Card title="USB link" hint="The cable to your computer (usb0)">
          <div class="form">
            <div class="form g2">
              <Field label="Camera address on USB" help="Factory default 172.32.0.93"><Input value={n.usb_ip ?? ''} onInput={(x) => net.set({ usb_ip: x })} placeholder="172.32.0.93" /></Field>
              <Field label="Netmask" help="Usually 255.255.255.0"><Input value={n.usb_netmask ?? ''} onInput={(x) => net.set({ usb_netmask: x })} placeholder="255.255.255.0" /></Field>
            </div>
            <Callout>The computer at the other end of the cable needs a fixed address in the same network, for example <b>{(n.usb_ip || '172.32.0.93').replace(/\.\d+$/, '.100')}</b> with netmask {n.usb_netmask || '255.255.255.0'}. After saving, reopen the page at the new address within 60 seconds and press “Keep new settings”, or the old address comes back by itself.</Callout>
          </div>
          <SaveBar dirty={net.dirty} onSave={() => net.save()} onReset={net.reload} />
        </Card>
        <Card title="Date & time">
          <div class="form">
            <div class="row">
              <div class="grow">
                <div class="dim small">Camera clock</div>
                <b>{new Date(Date.now() + clock).toLocaleString()}</b>
                {Math.abs(clock) > 5000 && <Badge kind="warn">{Math.round(clock / 1000)} s off</Badge>}
              </div>
              <Btn icon="clock" onClick={async () => {
                await api.post('/api/system/time', { epoch_ms: Date.now() });
                setClock(0);
                toast('Clock set from this computer', 'ok');
              }}>Sync with this computer</Btn>
            </div>
            <Field label="Time zone">
              <Select value={ZONES.some((z) => z[0] === t.tz_name) ? t.tz_name : 'custom'} onChange={(x) => {
                if (x === 'custom') return time.set({ tz_name: 'custom' });
                const z = ZONES.find((q) => q[0] === x)!;
                time.set({ tz_name: z[0], timezone: z[1] });
              }} options={[...ZONES.map((z) => [z[0], z[2]]), ['custom', 'Other (enter a POSIX time zone)…']] as any} />
            </Field>
            {!ZONES.some((z) => z[0] === t.tz_name) && (
              <Field label="POSIX time zone" help="Offset sign is inverted: IST-5:30 = UTC+5:30. With daylight saving: CET-1CEST,M3.5.0,M10.5.0/3. Examples: <-03>3, <+0545>-5:45">
                <Input mono value={t.timezone} onInput={(x) => time.set({ timezone: x, tz_name: 'custom' })} />
              </Field>
            )}
            <ToggleRow title="NTP time sync" desc="Needs internet or a local NTP server" checked={t.ntp !== false} onChange={(x) => time.set({ ntp: x })} />
            {t.ntp !== false && (
              <div class="form g2">
                <Field label="NTP server"><Input value={t.ntp_server} onInput={(x) => time.set({ ntp_server: x })} /></Field>
                <Field label="Interval"><Num value={t.ntp_interval_min} min={5} max={1440} onChange={(x) => time.set({ ntp_interval_min: x })} unit="min" /></Field>
              </div>
            )}
            {t.ntp !== false && <Btn sm kind="ghost" icon="refresh" onClick={() => api.post('/api/system/time', { ntp_now: true }).then(() => toast('Sync started'))}>Sync now</Btn>}
          </div>
          <SaveBar dirty={time.dirty} onSave={() => time.save()} onReset={time.reload} />
        </Card>
      </div>
    </div>
  );
}
