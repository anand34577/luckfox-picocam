import { useEffect, useState } from 'preact/hooks';
import { api, fmtBytes, fmtDuration } from '../api';
import { status, useStore } from '../store';
import { Icon } from '../ui/icons';
import { Badge, Card, Copy, Stat } from '../ui/kit';

export function DashboardPage() {
  const st = useStore(status);
  const [sum, setSum] = useState<any>(null);
  const [full, setFull] = useState<any>(null);
  useEffect(() => {
    const load = () => {
      api.get('/api/events/summary').then(setSum, () => {});
      api.get('/api/status').then(setFull, () => {});
    };
    load();
    const t = setInterval(load, 15000);
    return () => clearInterval(t);
  }, []);
  const sys = st?.system || full?.system;
  const vid = st?.video || full?.video;
  const sto = st?.storage || full?.storage;
  const ip = full?.network?.interfaces?.find((i: any) => i.ip)?.ip || location.hostname;
  const rtspPort = full?.rtsp_port && full.rtsp_port !== 554 ? `:${full.rtsp_port}` : '';
  if (!sys) return <div class="empty"><span class="spin" /></div>;
  const maxHour = Math.max(1, ...(sum?.hours || [0]));
  return (
    <div class="col">
      <div class="grid g4">
        <Stat icon="cpu" k="CPU" v={sys.cpu_pct} unit="%" pct={sys.cpu_pct} />
        <Stat icon="memory" k="Free memory" v={Math.round(sys.mem_avail_kb / 1024)} unit={`MB of ${Math.round(sys.mem_total_kb / 1024)}`} pct={100 - (sys.mem_avail_kb / sys.mem_total_kb) * 100} />
        <Stat icon="layers" k="Media memory (ISP/video/NPU)" v={Math.round(sys.media_mem_used_kb / 1024)} unit={`MB of ${Math.round(sys.media_mem_total_kb / 1024)}`} pct={(sys.media_mem_used_kb / sys.media_mem_total_kb) * 100} />
        <Stat icon="thermo" k="SoC temperature" v={sys.temp_c} unit="°C" pct={(sys.temp_c / 100) * 100} />
      </div>
      <div class="grid g3">
        <Card title="Streams" actions={<Badge kind="ok">{(full?.rtsp_sessions ?? st?.rtsp_sessions) || 0} RTSP viewer(s)</Badge>}>
          {vid && ['main', 'sub'].map((k) => (
            <div class="row between" style={{ padding: '6px 0' }}>
              <div>
                <b>{k === 'main' ? 'Main' : 'Sub'}</b> <span class="dim small">{vid[k].codec?.toUpperCase()} · {vid[k].width}×{vid[k].height}</span>
              </div>
              <span class="small muted">{vid[k].measured_fps} fps · {vid[k].kbps} kbps</span>
            </div>
          ))}
          <div class="section-title">Stream URLs</div>
          <div class="col" style={{ gap: 6 }}>
            <Copy text={`rtsp://${ip}${rtspPort}/live/main`} />
            <Copy text={`rtsp://${ip}${rtspPort}/live/sub`} />
          </div>
        </Card>
        <Card title="Today" hint={sum ? `${sum.today} events` : ''} actions={<a href="#/events" class="small">Events</a>}>
          <div class="hist" title="Events per hour">
            {(sum?.hours || Array(24).fill(0)).map((v: number) => <i style={{ height: `${(v / maxHour) * 100}%` }} title={String(v)} />)}
          </div>
          <div class="row small dim between"><span>0h</span><span>12h</span><span>23h</span></div>
          <div class="chips" style={{ marginTop: 12 }}>
            {sum && Object.entries(sum.by_label).map(([k, v]) => <span class="chip">{k} · {String(v)}</span>)}
            {sum && Object.entries(sum.by_type).map(([k, v]) => <span class="chip">{k} · {String(v)}</span>)}
          </div>
        </Card>
        <Card title="System">
          <dl class="kv">
            <dt>Uptime</dt><dd>{fmtDuration(sys.uptime_s)}</dd>
            <dt>Service uptime</dt><dd>{fmtDuration(sys.app_uptime_s)}</dd>
            <dt>Clock</dt><dd>{new Date(sys.time_ms).toLocaleString()} {sys.ntp_synced ? <Badge kind="ok">NTP</Badge> : <Badge kind="warn">no NTP</Badge>}</dd>
            <dt>Load</dt><dd>{sys.load?.map((x: number) => x.toFixed(2)).join(' · ')}</dd>
            <dt>App memory</dt><dd>{Math.round(sys.app_rss_kb / 1024)} MB</dd>
            {sys.npu_load_pct >= 0 && <><dt>NPU load</dt><dd>{sys.npu_load_pct}%</dd></>}
            <dt>Storage</dt><dd>{sto?.available ? `${fmtBytes(sto.free_bytes)} free of ${fmtBytes(sto.total_bytes)}` : 'none'} {sto?.recording && <Badge kind="bad"><Icon n="rec" s={10} /> REC</Badge>}</dd>
          </dl>
        </Card>
      </div>
    </div>
  );
}
