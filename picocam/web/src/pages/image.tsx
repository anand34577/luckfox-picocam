import { useEffect, useRef } from 'preact/hooks';
import { api } from '../api';
import { status, useStore } from '../store';
import { Badge, Btn, Callout, Card, Field, Modal, Num, Seg, Select, Slider, toast, ToggleRow, upd, useSection } from '../ui/kit';
import { useState } from 'preact/hooks';
import { LiveViewer } from '../ui/viewer';

const SCENE: Record<string, [string, string]> = {
  normal: ['Balanced', 'ok'], bright: ['Bright scene: protecting highlights', 'warn'], backlight: ['Backlight: lifting the subject + WDR', 'warn'],
  dark: ['Dark scene', ''], night: ['Night: maximum exposure', ''], manual: ['Manual', ''], auto: ['Standard AE', ''],
  subject: ['Exposing for the tracked object', 'ok'], night_lamps: ['Night with lights: not over-brightening', 'warn'],
};

const PRESETS: [string, string, string][] = [
  ['general', 'General', 'Balanced for most scenes'],
  ['anpr', 'ANPR / road', 'Short shutter, headlight protection, exposes for the vehicle'],
  ['indoor', 'Indoor', 'Even lighting, flicker-safe'],
  ['lowlight', 'Low light', 'More gain, stronger noise reduction, automatic day/night'],
];

// three pictures at -1 / 0 / +1 EV with their histograms
function ExposureTest(p: { onClose: () => void }) {
  const [res, setRes] = useState<any[] | null>(null);
  useEffect(() => {
    api.post('/api/image/exposure-test').then(setRes, (e) => (toast(e.message, 'err'), p.onClose()));
  }, []);
  return (
    <Modal wide title="Exposure test" onClose={p.onClose} footer={<Btn kind="primary" onClick={p.onClose}>Close</Btn>}>
      {!res ? <div class="empty"><span class="spin" /><div>Taking three pictures (about 6 s)…</div></div> : (
        <div class="grid g3">
          {res.map((r) => (
            <div class="col">
              <b>{r.offset > 0 ? '+' : ''}{r.offset} EV</b>
              {r.jpeg && <img src={`data:image/jpeg;base64,${r.jpeg}`} style={{ width: '100%', borderRadius: 8 }} />}
              {r.histogram && <div class="hist">{r.histogram.map((v: number) => <i style={{ height: `${(v / Math.max(1, ...r.histogram)) * 100}%` }} />)}</div>}
              <div class="small dim">median {r.median} · clipped {r.clip_hi_pct}% / dark {r.clip_lo_pct}% · {r.exposure_ms} ms ×{r.gain}</div>
            </div>
          ))}
        </div>
      )}
      <div class="small dim" style={{ marginTop: 10 }}>Compare the three: if the middle one is right, the controller is well tuned. If the −1 EV picture looks better, lower the target brightness; if +1 EV, raise it.</div>
    </Modal>
  );
}

// Image settings apply instantly (debounced) so the effect is visible in the live view.
export function ImagePage() {
  const { val, set, setVal, reload } = useSection<any>('image');
  const st = useStore(status);
  const timer = useRef<any>(null);
  const saveTimer = useRef<any>(null);
  const first = useRef(true);
  const [test, setTest] = useState(false);
  useEffect(() => {
    if (!val) return;
    if (first.current) {
      first.current = false;
      return;
    }
    clearTimeout(timer.current);
    clearTimeout(saveTimer.current);
    // live preview while dragging (not written to flash), saved once after 2 s without changes
    timer.current = setTimeout(() => api.put('/api/config/image?persist=0', val).catch((e) => toast(e.message, 'err')), 250);
    saveTimer.current = setTimeout(() => api.put('/api/config/image', val).catch(() => {}), 2000);
  }, [val]);
  if (!val) return <div class="empty"><span class="spin" /></div>;
  const ex = val.exposure || {};
  const isp = st?.isp;
  const E = (k: string, x: any) => set(custom(upd(val, `exposure.${k}`, x)));
  const reset = async () => {
    const d = await api.get('/api/config/defaults');
    setVal(d.image);
    toast('Defaults restored', 'ok');
  };
  // any hand edit of the exposure or noise settings turns the scene preset into "custom"
  const custom = (v: any) => upd(v, 'exposure.preset', 'custom');
  const dn = val.daynight || {};
  const tn = ex.tuning || {};
  const choosePreset = async (id: string) => {
    clearTimeout(timer.current);
    clearTimeout(saveTimer.current);
    try {
      setVal(await api.put('/api/config/image', { exposure: { preset: id } }));  // the camera applies the preset's settings
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  return (
    <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1.25fr) minmax(0,1fr)', alignItems: 'start' }}>
      <div class="col" style={{ position: 'sticky', top: 80 }}>
        <LiveViewer stream="sub" compact />
        <Card title="Exposure monitor" actions={isp && <Badge kind={SCENE[isp.scene]?.[1] || ''}>{SCENE[isp.scene]?.[0] || isp.scene}</Badge>}>
          {isp ? (
            <>
              <div class="hist">{isp.histogram.map((v: number) => <i style={{ height: `${(v / Math.max(1, ...isp.histogram)) * 100}%` }} />)}</div>
              <div class="row small dim between"><span>shadows</span><span>mid-tones</span><span>highlights</span></div>
              <div class="grid g4 mt" style={{ gap: 10 }}>
                <div><div class="dim small">Shutter</div><b>{isp.exposure_ms} ms</b></div>
                <div><div class="dim small">Gain</div><b>×{isp.gain}</b></div>
                <div><div class="dim small">Brightness</div><b>{isp.luma.median}</b></div>
                <div><div class="dim small">Clipped</div><b>{isp.luma.clip_hi_pct}%</b></div>
                <div><div class="dim small">EV bias</div><b>{isp.ev_bias > 0 ? '+' : ''}{isp.ev_bias}%</b></div>
                <div><div class="dim small">WDR gain</div><b>{isp.drc_gain}</b></div>
                <div><div class="dim small">Anti-flicker</div><b>{isp.anti_flicker ? 'on' : 'off'}</b></div>
                <div><div class="dim small">Light level</div><b>{isp.env_lv}</b></div>
                <div><div class="dim small">Shutter limit</div><b>{isp.shutter_cap_ms} ms</b></div>
                <div><div class="dim small">Mode</div><b>{isp.night ? 'night (B/W)' : 'day'}</b></div>
                {isp.health && (
                  <div title={isp.health.last_fault ? `Last automatic recovery: ${isp.health.last_fault}` : 'Watches for a green picture or a stuck exposure and restarts the video pipeline if one appears'}>
                    <div class="dim small">Picture health</div>
                    <b>{isp.health.state === 'ok' ? 'ok' : isp.health.state.replace('_', ' ')}{isp.health.recoveries ? ` · ${isp.health.recoveries} recovered` : ''}</b>
                  </div>
                )}
                <div><div class="dim small">Metering</div><b>{isp.subject ? 'on object' : 'scene'}</b></div>
                {isp.wb && <div><div class="dim small">Colour temp.</div><b>{isp.wb.cct ? `${isp.wb.cct} K` : '—'}</b></div>}
                {isp.wb?.smart && <div><div class="dim small">WB correction</div><b>R×{isp.wb.correction_r} B×{isp.wb.correction_b}</b></div>}
              </div>
            </>
          ) : <span class="dim">waiting for data…</span>}
        </Card>
      </div>
      <div class="col">
        <Card title="Scene" hint="One click sets exposure, noise reduction and WDR for the place the camera watches">
          <div class="grid g4" style={{ gap: 10 }}>
            {PRESETS.map(([id, title, desc]) => (
              <div class={'card' + (ex.preset === id ? ' on' : '')} role="button" tabIndex={0} style={{ padding: 12, cursor: 'pointer', borderColor: ex.preset === id ? 'var(--accent)' : undefined }} onClick={() => choosePreset(id)}>
                <b>{title}</b>
                <div class="small muted" style={{ marginTop: 4 }}>{desc}</div>
              </div>
            ))}
          </div>
          {ex.preset === 'custom' && <div class="small dim" style={{ marginTop: 8 }}>Custom settings (a preset was adjusted by hand).</div>}
        </Card>
        <Card title="Exposure" actions={<><Btn sm kind="ghost" icon="play" onClick={() => setTest(true)}>Test</Btn><Btn sm kind="ghost" onClick={reset}>Restore defaults</Btn></>}>
          <div class="form">
            <Field label="Mode" help={ex.mode === 'smart' ? 'Smart Exposure reads the scene histogram: protects sky and lamps, lifts backlit subjects, and only uses anti-flicker indoors.' : ex.mode === 'auto' ? 'Rockchip standard auto exposure' : 'Fixed shutter and gain'}>
              <Seg value={ex.mode} onChange={(x) => E('mode', x)} options={[['smart', 'Smart'], ['auto', 'Standard'], ['manual', 'Manual']]} />
            </Field>
            {ex.mode !== 'manual' ? (
              <>
                <Field label="Target brightness"><Slider value={ex.target ?? 50} onChange={(x) => E('target', x)} /></Field>
                <Field label="Exposure compensation"><Slider value={ex.compensation ?? 0} min={-100} max={100} onChange={(x) => E('compensation', x)} /></Field>
                <div class="form g2">
                  <Field label="Longest shutter" help="Longer = brighter at night, more motion blur"><Select value={String(ex.max_shutter_ms)} onChange={(x) => E('max_shutter_ms', +x)} options={[['5', '1/200 s'], ['10', '1/100 s'], ['20', '1/50 s'], ['33', '1/30 s'], ['40', '1/25 s']]} /></Field>
                  <Field label="Maximum gain" help="Higher = brighter at night, more noise"><Select value={String(ex.max_gain)} onChange={(x) => E('max_gain', +x)} options={[['8', '×8'], ['16', '×16'], ['32', '×32'], ['64', '×64'], ['128', '×128']]} /></Field>
                  <Field label="Metering"><Select value={ex.metering} onChange={(x) => E('metering', x)} options={[['center', 'Centre-weighted'], ['average', 'Average'], ['spot', 'Spot'], ['lower', 'Lower half (driveway/road)'], ['upper', 'Upper half']]} /></Field>
                  <Field label="Reaction speed"><Seg value={ex.speed} onChange={(x) => E('speed', x)} options={[['slow', 'Slow'], ['normal', 'Normal'], ['fast', 'Fast']]} /></Field>
                </div>
              </>
            ) : (
              <div class="form g2">
                <Field label="Shutter"><Num value={ex.shutter_ms} min={0.05} max={40} step={0.05} onChange={(x) => E('shutter_ms', x)} unit="ms" /></Field>
                <Field label="Gain"><Num value={ex.gain} min={1} max={128} step={0.5} onChange={(x) => E('gain', x)} unit="×" /></Field>
              </div>
            )}
            {ex.mode === 'smart' && (
              <>
                <ToggleRow title="Expose for tracked objects" desc="With AI on, the exposure follows the vehicle, person or face being tracked instead of the whole scene (headlights and bright skies stop fooling it)."
                  checked={!!ex.ai_roi} onChange={(x) => E('ai_roi', x)} />
                <ToggleRow title="Motion shutter" desc="Shortens the shutter while something fast is in view so plates and faces are not blurred; relaxes again afterwards."
                  checked={!!ex.motion_shutter} onChange={(x) => E('motion_shutter', x)} />
                {ex.motion_shutter && <Field label="Allowed blur" help="Pixels of motion blur on the analysis picture"><Slider value={ex.blur_px ?? 3} min={1} max={10} step={0.5} unit=" px" onChange={(x) => E('blur_px', x)} /></Field>}
              </>
            )}
            {ex.mode !== 'manual' && <Field label="Priority" help="Highlight priority protects lamps and headlights; low-light priority protects dark areas"><Seg value={ex.strategy || 'lowlight'} onChange={(x) => E('strategy', x)} options={[['lowlight', 'Dark areas'], ['highlight', 'Highlights']]} /></Field>}
            <Field label="Anti-flicker" help="Auto: removes banding under mains-powered lights, switches off in daylight (avoids over-exposure)">
              <Seg value={ex.anti_flicker} onChange={(x) => E('anti_flicker', x)} options={[['auto', 'Auto'], ['50hz', '50 Hz'], ['60hz', '60 Hz'], ['off', 'Off']]} />
            </Field>
          </div>
        </Card>
        <Card title="Dynamic range">
          <div class="form">
            <Field label="WDR (digital)" help="Lifts shadows in high-contrast scenes"><Seg value={val.wdr?.mode} onChange={(x) => set(upd(val, 'wdr.mode', x))} options={[['auto', 'Auto'], ['on', 'On'], ['off', 'Off']]} /></Field>
            {val.wdr?.mode !== 'off' && <Field label={val.wdr?.mode === 'auto' ? 'Maximum WDR strength' : 'WDR strength'}><Slider value={val.wdr?.level ?? 50} onChange={(x) => set(upd(val, 'wdr.level', x))} /></Field>}
            <Field label="Backlight / highlight compensation"><Seg value={val.backlight} onChange={(x) => set(custom({ ...val, backlight: x }))} options={[['off', 'Off'], ['blc', 'BLC'], ['hlc', 'HLC (headlights)']]} /></Field>
            <Field label="Defog"><Seg value={val.dehaze?.mode} onChange={(x) => set(upd(val, 'dehaze.mode', x))} options={[['off', 'Off'], ['on', 'On']]} /></Field>
            {val.dehaze?.mode === 'on' && <Field label="Defog strength"><Slider value={val.dehaze?.level ?? 50} onChange={(x) => set(upd(val, 'dehaze.level', x))} /></Field>}
          </div>
        </Card>
        <Card title="Picture">
          <div class="form">
            {(['brightness', 'contrast', 'saturation', 'sharpness', 'hue'] as const).map((k) => (
              <Field label={k[0].toUpperCase() + k.slice(1)}><Slider value={val[k] ?? 50} onChange={(x) => set({ [k]: x })} /></Field>
            ))}
            <div class="form g2">
              <Field label="Mirror"><Seg value={val.mirror ? '1' : '0'} onChange={(x) => set({ mirror: x === '1' })} options={[['0', 'Off'], ['1', 'On']]} /></Field>
              <Field label="Flip"><Seg value={val.flip ? '1' : '0'} onChange={(x) => set({ flip: x === '1' })} options={[['0', 'Off'], ['1', 'On']]} /></Field>
            </div>
          </div>
        </Card>
        <Card title="Colour & noise">
          <div class="form">
            <Field label="White balance" help="Smart auto corrects warm room lamps and mixed lamp + daylight scenes on top of the sensor's AWB"><Select value={val.wb?.mode} onChange={(x) => set(upd(val, 'wb.mode', x))} options={[['auto', 'Smart auto'], ['basic', 'Basic auto'], ['daylight', 'Daylight'], ['cloudy', 'Cloudy'], ['shade', 'Shade'], ['fluorescent', 'Fluorescent'], ['incandescent', 'Incandescent'], ['manual', 'Manual']]} /></Field>
            {val.wb?.mode === 'manual' && (['r', 'g', 'b'] as const).map((c) => (
              <Field label={{ r: 'Red', g: 'Green', b: 'Blue' }[c] + ' gain'}><Slider value={val.wb?.[c] ?? 50} onChange={(x) => set(upd(val, `wb.${c}`, x))} /></Field>
            ))}
            <Field label="Spatial noise reduction"><Slider value={val.nr?.spatial ?? 50} onChange={(x) => set(custom(upd(val, 'nr.spatial', x)))} /></Field>
            <Field label="Temporal noise reduction" help="Higher = cleaner night picture, more ghosting on motion"><Slider value={val.nr?.temporal ?? 50} onChange={(x) => set(custom(upd(val, 'nr.temporal', x)))} /></Field>
            <Field label="Day / night" help="Auto switches to black & white (and the IR light) when the scene gets too dark for colour, and back by day">
              <Seg value={dn.mode || 'color'} onChange={(x) => set(upd(val, 'daynight.mode', x))} options={[['color', 'Colour'], ['bw', 'Black & white'], ['auto', 'Automatic']]} />
            </Field>
            {dn.mode === 'auto' && (
              <>
                <div class="form g2">
                  <Field label="Night below light level" help={`0 = decide from exposure. Now: ${isp?.env_lv ?? '–'}`}><Num value={dn.night_lv ?? 0} min={0} step={0.1} onChange={(x) => set(upd(val, 'daynight.night_lv', x))} /></Field>
                  <Field label="Day above light level" help="0 = 1.5 × the night level"><Num value={dn.day_lv ?? 0} min={0} step={0.1} onChange={(x) => set(upd(val, 'daynight.day_lv', x))} /></Field>
                  <Field label="IR-cut filter pin" help="GPIO number, −1 = none"><Num value={dn.ircut_gpio ?? -1} min={-1} max={511} onChange={(x) => set(upd(val, 'daynight.ircut_gpio', x))} /></Field>
                  <Field label="IR light pin" help="GPIO number, −1 = none"><Num value={dn.irled_gpio ?? -1} min={-1} max={511} onChange={(x) => set(upd(val, 'daynight.irled_gpio', x))} /></Field>
                </div>
                <ToggleRow title="Invert the IR-cut pin" desc="Pin high = filter out (night) instead of in" checked={!!dn.ircut_invert} onChange={(x) => set(upd(val, 'daynight.ircut_invert', x))} />
                <Callout>The camera never switches more often than once a minute. An IR light can brighten the picture enough to look like day: if it flips back, set a day level well above the night one.</Callout>
              </>
            )}
          </div>
        </Card>
        {ex.mode === 'smart' && (
          <Card title="Controller tuning" hint="Advanced: the Smart Exposure constants for this site">
            <div class="form g2">
              <Field label="Reaction strength" help="Share of the error corrected per step"><Slider value={Math.round((tn.gain ?? 0.4) * 100)} min={10} max={90} onChange={(x) => E('tuning', { ...tn, gain: x / 100 })} unit="%" /></Field>
              <Field label="Dead band" help="Errors smaller than this (in stops) are ignored"><Slider value={Math.round((tn.deadband ?? 0.2) * 100)} min={5} max={80} onChange={(x) => E('tuning', { ...tn, deadband: x / 100 })} unit="/100 EV" /></Field>
              <Field label="Backlight: highlights" help="Share of bright pixels that, with dark ones, means backlight"><Slider value={Math.round((tn.backlight_hi ?? 0.03) * 1000) / 10} min={0.5} max={30} step={0.5} onChange={(x) => E('tuning', { ...tn, backlight_hi: x / 100 })} unit="%" /></Field>
              <Field label="Backlight: shadows"><Slider value={Math.round((tn.backlight_lo ?? 0.18) * 100)} min={5} max={80} onChange={(x) => E('tuning', { ...tn, backlight_lo: x / 100 })} unit="%" /></Field>
            </div>
          </Card>
        )}
        <div class="dim small">Changes apply immediately and are saved automatically. <a onClick={reload}>Reload</a></div>
        {test && <ExposureTest onClose={() => setTest(false)} />}
      </div>
    </div>
  );
}
