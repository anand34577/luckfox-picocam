import { useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { ShapeEditor } from '../ui/editor';
import { Callout, Card, Field, Num, SaveBar, Seg, Select, ToggleRow, Input, upd, useSection } from '../ui/kit';
import { useSnapshotUrl } from '../ui/viewer';

const STD_MAIN: [number, number, string][] = [[2880, 1620, '5 MP'], [2560, 1440, '4 MP'], [2304, 1296, '3 MP'], [1920, 1080, '1080p'], [1280, 720, '720p'], [960, 540, '']];

// the main stream can be as large as the camera module delivers (3 MP SC3336, 4 MP SC4336, 5 MP SC530AI, 2 MP ...)
function mainResolutions(sw: number, sh: number, current: string): [string, string][] {
  const out: [string, string][] = [];
  const add = (w: number, h: number, tag: string) => {
    const k = `${w}x${h}`;
    if (!out.some((o) => o[0] === k)) out.push([k, `${w}×${h}${tag ? ` (${tag})` : ''}`]);
  };
  if (sw > 0 && sh > 0) add(sw, sh, `${Math.round((sw * sh) / 1e6 * 10) / 10} MP, sensor`);
  for (const [w, h, t] of STD_MAIN) if (!sw || (w <= sw && h <= sh)) add(w, h, t);
  if (current && !out.some((o) => o[0] === current)) out.unshift([current, current.replace('x', '×')]);
  return out;
}
const SUB_RES: [string, string][] = [['1280x720', '1280×720'], ['960x540', '960×540'], ['704x396', '704×396'], ['640x360', '640×360']];

function StreamForm(p: { k: 'main' | 'sub'; v: any; sensorFps: number; sensor: [number, number]; onChange: (v: any) => void }) {
  const v = p.v;
  const set = (k: string, x: any) => p.onChange({ ...v, [k]: x });
  const res = `${v.width}x${v.height}`;
  return (
    <div class="form g2">
      <div class="span2"><ToggleRow title="Stream enabled" checked={v.enabled !== false} onChange={(x) => set('enabled', x)} /></div>
      <Field label="Codec" help={v.codec === 'h265' ? 'Half the bitrate; browsers need hardware HEVC for live view' : 'Plays everywhere'}>
        <Seg value={v.codec} onChange={(x) => set('codec', x)} options={[['h264', 'H.264'], ['h265', 'H.265']]} />
      </Field>
      <Field label="Resolution">
        <Select value={res} onChange={(x) => {
          const [w, h] = x.split('x').map(Number);
          p.onChange({ ...v, width: w, height: h });
        }} options={p.k === 'main' ? mainResolutions(p.sensor[0], p.sensor[1], res) : SUB_RES} />
      </Field>
      <Field label="Frame rate"><Num value={v.fps} min={1} max={p.sensorFps} onChange={(x) => set('fps', x)} unit="fps" /></Field>
      <Field label="Keyframe interval (GOP)" help="Shorter = faster start and seeking, more bitrate"><Num value={v.gop} min={1} max={600} onChange={(x) => set('gop', x)} unit="frames" /></Field>
      <Field label="Bitrate control"><Seg value={v.rc} onChange={(x) => set('rc', x)} options={[['cbr', 'Constant'], ['vbr', 'Variable'], ['avbr', 'Adaptive']]} /></Field>
      <Field label={v.rc === 'cbr' ? 'Bitrate' : 'Target bitrate'}><Num value={v.bitrate} min={64} max={20000} onChange={(x) => set('bitrate', x)} unit="kbps" /></Field>
      {v.rc !== 'cbr' && <Field label="Maximum bitrate"><Num value={v.max_bitrate} min={v.bitrate} max={30000} onChange={(x) => set('max_bitrate', x)} unit="kbps" /></Field>}
      <Field label="Quality"><Select value={v.quality} onChange={(x) => set('quality', x)} options={[['highest', 'Highest'], ['high', 'High'], ['medium', 'Medium'], ['low', 'Low']]} /></Field>
      {v.codec === 'h264' && <Field label="H.264 profile"><Select value={v.profile} onChange={(x) => set('profile', x)} options={[['high', 'High'], ['main', 'Main'], ['baseline', 'Baseline']]} /></Field>}
      <div class="span2">
        <ToggleRow title="Smart encoding (SmartP)" desc="Long-term reference frames: much lower bitrate for static scenes" checked={!!v.smart_p} onChange={(x) => set('smart_p', x)} />
        {p.k === 'sub' && <ToggleRow title="Burn AI boxes into the sub stream" desc="For NVRs and players that can't show the web overlay" checked={!!v.ai_overlay} onChange={(x) => set('ai_overlay', x)} />}
      </div>
    </div>
  );
}

export function VideoPage() {
  const video = useSection<any>('video');
  const osd = useSection<any>('osd');
  const masks = useSection<any[]>('masks');
  const [img, refresh] = useSnapshotUrl();
  const [sensor, setSensor] = useState<[number, number]>([0, 0]);
  useEffect(() => {
    api.get('/api/status').then((s) => setSensor([s.device?.board?.sensor_width || 0, s.device?.board?.sensor_height || 0]), () => {});
  }, []);
  const v = video.val, o = osd.val;
  if (!v || !o || !masks.val) return <div class="empty"><span class="spin" /></div>;
  const POS: [string, string][] = [['top-left', 'Top left'], ['top-right', 'Top right'], ['bottom-left', 'Bottom left'], ['bottom-right', 'Bottom right']];
  return (
    <div class="col">
      <div class="grid g2">
        <Card title="Main stream" hint="Recording & NVR">
          <StreamForm k="main" v={v.main} sensorFps={v.sensor_fps} sensor={sensor} onChange={(x) => video.set({ main: x })} />
        </Card>
        <Card title="Sub stream" hint="Web live view, mobile, AI analysis">
          <StreamForm k="sub" v={v.sub} sensorFps={v.sensor_fps} sensor={sensor} onChange={(x) => video.set({ sub: x })} />
          <div class="mt"><Callout>AI analyses the sub stream: 960×540 is a good balance for plates and people. Higher costs more CPU per frame.</Callout></div>
        </Card>
      </div>
      <Card title="Sensor">
        <div class="form g2">
          <Field label="Sensor frame rate" help="Upper limit for both streams; 25 fps matches 50 Hz lighting"><Seg value={String(v.sensor_fps)} onChange={(x) => video.set({ sensor_fps: +x })} options={[['15', '15'], ['20', '20'], ['25', '25'], ['30', '30']]} /></Field>
          <Field label="Snapshot JPEG quality"><Num value={v.jpeg?.quality ?? 80} min={10} max={99} onChange={(x) => video.set(upd(v, 'jpeg.quality', x))} /></Field>
        </div>
      </Card>
      <SaveBar dirty={video.dirty} onSave={() => video.save()} onReset={video.reload} note="Saving restarts the encoders (viewers reconnect automatically)" />

      <div class="grid g2">
        <Card title="On-screen display">
          <div class="form">
            <ToggleRow title="Date & time" checked={o.time?.enabled !== false} onChange={(x) => osd.set(upd(o, 'time.enabled', x))} />
            <div class="form g2">
              <Field label="Position"><Select value={o.time?.position} onChange={(x) => osd.set(upd(o, 'time.position', x))} options={POS} /></Field>
              <Field label="Format"><Select value={o.time?.format} onChange={(x) => osd.set(upd(o, 'time.format', x))} options={[
                ['%Y-%m-%d %H:%M:%S', '2026-01-31 23:59:59'], ['%d-%m-%Y %H:%M:%S', '31-01-2026 23:59:59'], ['%d/%m/%Y %I:%M:%S %p', '31/01/2026 11:59:59 PM'], ['%a %d %b %Y %H:%M:%S', 'Sat 31 Jan 2026 23:59:59']]} /></Field>
            </div>
            <ToggleRow title="Camera name" checked={o.name?.enabled !== false} onChange={(x) => osd.set(upd(o, 'name.enabled', x))} />
            <div class="form g2">
              <Field label="Text" help="Empty = device name"><Input value={o.name?.text} onInput={(x) => osd.set(upd(o, 'name.text', x))} /></Field>
              <Field label="Position"><Select value={o.name?.position} onChange={(x) => osd.set(upd(o, 'name.position', x))} options={POS} /></Field>
            </div>
            <Field label="Size"><Seg value={o.size} onChange={(x) => osd.set({ size: x })} options={[['small', 'Small'], ['medium', 'Medium'], ['large', 'Large']]} /></Field>
          </div>
          <SaveBar dirty={osd.dirty} onSave={() => osd.save()} onReset={osd.reload} />
        </Card>
        <Card title="Privacy masks" hint="Up to 8 areas blacked out in every stream, snapshot and recording">
          <ShapeEditor img={img} onRefresh={refresh} mode="rects" value={masks.val} onChange={(x) => masks.setVal(x)} />
          <SaveBar dirty={masks.dirty} onSave={() => masks.save(true)} onReset={masks.reload} />
        </Card>
      </div>
    </div>
  );
}
