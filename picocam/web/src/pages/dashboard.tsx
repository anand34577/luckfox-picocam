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
        <Stat icon="cpu" k="CPU" v={sys.cpu_pct} unit={sys.cpu_mhz > 0 ? `% · ${sys.cpu_mhz} MHz` : '%'} pct={sys.cpu_pct} />
        <Stat icon="memory" k="Free memory" v={Math.round(sys.mem_avail_kb / 1024)} unit={`MB of ${Math.round(sys.mem_total_kb / 1024)}`} pct={100 - (sys.mem_avail_kb / sys.mem_total_kb) * 100} />
        <Stat icon="layers" k="Media memory (ISP/video/NPU)" v={Math.round(sys.media_mem_used_kb / 1024)} unit={`MB of ${Math.round(sys.media_mem_total_kb / 1024)}`} pct={(sys.media_mem_used_kb / sys.media_mem_total_kb) * 100} />
        <Stat icon="thermo" k="SoC temperature" v={sys.temp_c} unit="°C" pct={(sys.temp_c / 100) * 100} />
      </div>
      <div class="grid g4">
        <Stat icon="cpu" k="PicoCam CPU" v={sys.app_cpu_pct ?? '–'} unit={sys.cpu_user_pct !== undefined ? `% · board ${sys.cpu_user_pct}% user, ${sys.cpu_sys_pct}% kernel` : '%'} pct={sys.app_cpu_pct} />
        <Stat icon="layers" k="NPU (AI accelerator)" v={sys.npu_load_pct >= 0 ? sys.npu_load_pct : '–'} unit={st?.ai?.running ? `% · ${st.ai.fps} fps · ${st.ai.npu?.inference_ms ?? 0} ms/run` : '%'} pct={sys.npu_load_pct >= 0 ? sys.npu_load_pct : undefined} />
        <Stat icon="layers" k="RGA (2D engine)" v={sys.rga_load_pct >= 0 ? sys.rga_load_pct : '–'} unit={st?.ai?.npu ? `% · prepare ${st.ai.npu.preprocess}, copy ${st.ai.npu.frame_copy ?? 'cpu'}` : '%'} pct={sys.rga_load_pct >= 0 ? sys.rga_load_pct : undefined} />
        <Stat icon="cpu" k="Capture thread" v={vid?.capture ? vid.capture.frame_ms : '–'} unit={vid?.capture && vid.sub?.fps ? `ms per frame of ${Math.round(1000 / vid.sub.fps)}` : ''} pct={vid?.capture && vid.sub?.fps ? (vid.capture.frame_ms * vid.sub.fps) / 10 : undefined} />
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
            <dt>Load average</dt><dd title="Linux load average. On Rockchip boards about 10 idle media-driver threads always count towards it, so it is not CPU use: see CPU above">{sys.load?.map((x: number) => x.toFixed(2)).join(' · ')}</dd>
            <dt>App memory</dt><dd>{Math.round(sys.app_rss_kb / 1024)} MB</dd>
            <dt>Storage</dt><dd>{sto?.available ? `${fmtBytes(sto.free_bytes)} free of ${fmtBytes(sto.total_bytes)}` : 'none'} {sto?.recording && <Badge kind="bad"><Icon n="rec" s={10} /> REC</Badge>}</dd>
          </dl>
        </Card>
      </div>
    </div>
  );
}
