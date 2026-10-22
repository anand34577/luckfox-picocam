import { useEffect, useState } from 'preact/hooks';
import { api, fmtBytes, uploadWithProgress } from '../api';
import { isAdmin, status, useStore } from '../store';
import { ShapeEditor } from '../ui/editor';
import { Icon } from '../ui/icons';
import {
  Badge, Btn, Callout, Card, Chips, confirmDialog, Empty, Field, Input, Modal, Num, SaveBar, Select, Slider, Switch, Tabs, toast,
  ToggleRow, upd, useSection,
} from '../ui/kit';
import { Overlay, useSnapshotUrl } from '../ui/viewer';
import { Schedule } from './storage';

const TASKS: [string, string][] = [
  ['detect', 'Object detector (YOLO family, custom decoder)'],
  ['face_detect', 'Face detector (RetinaFace)'],
  ['classify', 'Classifier'],
  ['ocr', 'Text / plate reader (CTC)'],
  ['face_embed', 'Face embedding'],
];
const TASK_SHORT: Record<string, string> = { detect: 'Detector', face_detect: 'Face detector', classify: 'Classifier', ocr: 'OCR', face_embed: 'Embedding' };
const STAGE_TYPES: [string, string][] = [['detect', 'Find a part (detect)'], ['ocr', 'Read text (OCR)'], ['classify', 'Classify'], ['embed', 'Face embedding'], ['raw', 'Send raw outputs to a server']];
const STAGE_TASK: Record<string, string[]> = { detect: ['detect'], ocr: ['ocr'], classify: ['classify'], embed: ['face_embed'], raw: ['detect', 'classify', 'ocr', 'face_embed', 'face_detect'] };
const DECODERS: [string, string][] = [['auto', 'Automatic'], ['yolov5', 'YOLOv5 / v7 (anchors)'], ['yolov8', 'YOLOv8 / YOLO11 (DFL)'], ['yolox', 'YOLOX'], ['yolov10', 'YOLOv10 / end-to-end'], ['plugin', 'My own decoder plugin']];

function useModels() {
  const [models, setModels] = useState<any[]>([]);
  const load = () => api.get('/api/ai/models').then(setModels, (e) => toast(e.message, 'err'));
  useEffect(() => {
    load();
  }, []);
  return { models, reload: load };
}

const labelsOf = (models: any[], id: string): string[] => models.find((m) => m.id === id)?.meta?.labels || [];

// ---------------------------------------------------------------- templates

const VEHICLES = ['car', 'motorcycle', 'bus', 'truck'];
export const TEMPLATES: { id: string; title: string; desc: string; icon: string; ai: any; rules?: any[]; image?: string }[] = [
  {
    id: 'people', title: 'People & vehicle detection', icon: 'person',
    desc: 'COCO detector for people, cars, bikes, buses and trucks. Add zone or line rules for alerts.',
    ai: { detector: { model: 'yolov5n_coco_320.rknn', conf: 0.4, nms: 0.45, classes: ['person', 'bicycle', ...VEHICLES], min_box_px: 12 }, stages: [] },
    rules: [{ id: 'presence-person', name: 'Person detected', type: 'presence', classes: ['person'], cooldown_s: 30, enabled: true }],
    image: 'general',
  },
  {
    id: 'anpr', title: 'ANPR · vehicle type + number plate', icon: 'car',
    desc: 'Vehicles → plate detector → Indian plate OCR, voted per character over the track. One record per vehicle (repeats within 30 s are merged). Also sets the ANPR exposure preset. Needs 28 MB media memory with the 3 MP main stream.',
    ai: {
      detector: { model: 'yolov5n_coco_320.rknn', conf: 0.35, nms: 0.45, classes: VEHICLES, min_box_px: 40 },
      stages: [{ name: 'plate', type: 'detect', model: 'plate_yolo11n_320.rknn', on: VEHICLES, conf: 0.3, min_px: 20, interval_ms: 150,
        then: { name: 'plate_text', type: 'ocr', model: 'platenet_in.rknn', format: 'indian', conf: 0.3, votes: 3 } }],
    },
    rules: [{ id: 'anpr-log', name: 'Vehicle log', type: 'track', classes: VEHICLES, dedupe_s: 30, dedupe_attr: 'plate_text', enabled: true }],
    image: 'anpr',
  },
  {
    id: 'face', title: 'Face capture & recognition', icon: 'user',
    desc: 'RetinaFace finds faces, MobileFaceNet aligns them on landmarks and computes embeddings. Enrol people in the Faces tab to recognise them on the camera.',
    ai: { detector: { model: 'retinaface_320.rknn', conf: 0.6, classes: [] }, stages: [{ name: 'face_id', type: 'embed', model: 'mobilefacenet.rknn', on: ['face'], interval_ms: 300, gallery: true, threshold: 0.45 }] },
    rules: [{ id: 'face-log', name: 'Face captured', type: 'track', classes: ['face'], enabled: true }],
    image: 'general',
  },
  {
    id: 'count', title: 'People counting (line)', icon: 'line',
    desc: 'Counts people crossing a line in both directions. Draw the line in Rules after applying.',
    ai: { detector: { model: 'yolov5n_coco_320.rknn', conf: 0.4, nms: 0.45, classes: ['person'], min_box_px: 12 }, stages: [] },
    rules: [{ id: 'count-line', name: 'Entrance counter', type: 'line_cross', classes: ['person'], line: [[0.3, 0.6], [0.7, 0.6]], direction: 'any', cooldown_s: 0, reset_daily: true, enabled: true }],
    image: 'indoor',
  },
];

export async function applyTemplate(t: (typeof TEMPLATES)[0]) {
  await api.put('/api/config/ai', { ...t.ai, enabled: true });
  if (t.rules) {
    const cur: any[] = await api.get('/api/config/rules');
    const ids = new Set(cur.map((r) => r.id));
    await api.post('/api/config/rules', [...cur, ...t.rules.filter((r) => !ids.has(r.id))]);
  }
  if (t.image) await api.put('/api/config/image', { exposure: { preset: t.image } });
}

// ---------------------------------------------------------------- pipeline tab

function StageEditor(p: { s: any; models: any[]; parentLabels: string[]; onChange: (s: any) => void; onRemove?: () => void; nested?: boolean }) {
  const s = p.s;
  const set = (k: string, v: any) => p.onChange({ ...s, [k]: v });
  const opts = p.models.filter((m) => (STAGE_TASK[s.type] || []).includes(m.task));
  return (
    <div class="stage-card">
      <div class="row" style={{ marginBottom: 12 }}>
        <Badge kind="accent">{p.nested ? 'then' : 'stage'}</Badge>
        <b>{s.name || s.type}</b>
        <span class="grow" />
        {!p.nested && <label class="row small muted" style={{ gap: 8 }}><Switch checked={s.enabled !== false} onChange={(v) => set('enabled', v)} /> enabled</label>}
        {p.onRemove && <Btn sm kind="ghost" icon="trash" onClick={p.onRemove} />}
      </div>
      <div class="form g2">
        <Field label="Step"><Select value={s.type} onChange={(v) => p.onChange({ ...s, type: v, model: '' })} options={STAGE_TYPES} /></Field>
        <Field label="Result name" help="Attribute name in events, e.g. plate, plate_text, colour"><Input value={s.name} onInput={(v) => set('name', v.replace(/[^a-z0-9_]/gi, '_'))} /></Field>
        <Field label="Model"><Select value={s.model} onChange={(v) => set('model', v)} options={[['', '— choose —'], ...opts.map((m) => [m.id, m.name])] as any} /></Field>
        <Field label="Minimum confidence"><Slider value={Math.round((s.conf ?? 0.3) * 100)} onChange={(v) => set('conf', v / 100)} unit="%" /></Field>
        {!p.nested && (
          <Field label="Run on" class="span2" help="Objects from the detector this step looks at (empty = all)">
            <Chips all={p.parentLabels} value={s.on || []} onChange={(v) => set('on', v)} empty="detector has no labels" />
          </Field>
        )}
        {!p.nested && <Field label="Re-check every" help="Per object, until the result is certain"><Num value={s.interval_ms ?? 200} onChange={(v) => set('interval_ms', v)} min={0} unit="ms" /></Field>}
        {s.type === 'detect' && <Field label="Minimum part width"><Num value={s.min_px ?? 0} onChange={(v) => set('min_px', v)} min={0} unit="px" /></Field>}
        {s.type === 'ocr' && (
          <>
            <Field label="Plate format" help="Indian: fixes O/0, I/1, B/8… by position"><Select value={s.format || 'none'} onChange={(v) => set('format', v)} options={[['none', 'Generic text'], ['indian', 'Indian number plates']]} /></Field>
            <Field label="Certainty (votes)" help="Accumulated confidence before a reading is final (voted per character)"><Num value={s.votes ?? 3} onChange={(v) => set('votes', v)} min={0.5} step={0.5} /></Field>
          </>
        )}
        {s.type === 'classify' && <Field label="Certainty (votes)"><Num value={s.votes ?? 3} onChange={(v) => set('votes', v)} min={0.5} step={0.5} /></Field>}
        {s.type === 'embed' && (
          <>
            <div class="span2"><ToggleRow title="Recognise enrolled people" desc="Compare each face with the Faces tab. The result is the attribute “identity” (a name, or “unknown”)." checked={s.gallery !== false} onChange={(v) => set('gallery', v)} /></div>
            {s.gallery !== false && <Field label="Match threshold" help="Higher = stricter. 45 % suits MobileFaceNet."><Slider value={Math.round((s.threshold ?? 0.45) * 100)} min={20} max={80} onChange={(v) => set('threshold', v / 100)} unit="%" /></Field>}
          </>
        )}
        {s.type === 'raw' && (
          <>
            <Field label="Server URL" class="span2" help="Receives the model’s raw output tensors as JSON (base64). It may answer an object with “attributes”, which join the object’s attributes."><Input mono value={s.url} placeholder="https://server.example/decode" onInput={(v) => set('url', v)} /></Field>
            <Field label="Certificate check"><Select value={s.verify || 'system'} onChange={(v) => set('verify', v)} options={[['system', 'Trusted CA'], ['pin', 'Pin fingerprint'], ['none', 'Skip (insecure)']]} /></Field>
            {s.verify === 'pin' && <Field label="SHA-256 fingerprint"><Input mono value={s.pin} onInput={(v) => set('pin', v)} /></Field>}
          </>
        )}
      </div>
      {s.type === 'detect' && (
        <div style={{ marginTop: 12 }}>
          {s.then ? (
            <StageEditor nested s={s.then} models={p.models} parentLabels={[]} onChange={(t) => set('then', t)} onRemove={() => set('then', undefined)} />
          ) : (
            <Btn sm kind="ghost" icon="plus" onClick={() => set('then', { name: 'text', type: 'ocr', model: '', conf: 0.3, votes: 3 })}>Then read / classify the found part</Btn>
          )}
        </div>
      )}
    </div>
  );
}

function ProfilesCard(p: { val: any; set: (v: any) => void; edit: string; setEdit: (v: string) => void }) {
  const { val, set, edit, setEdit } = p;
  const profiles: any[] = val.profiles || [];
  const [sched, setSched] = useState<string | null>(null);
  const st = useStore(status);
  const add = () => {
    const id = 'p' + Math.random().toString(16).slice(2, 6);
    const name = `Profile ${profiles.length + 1}`;
    const base = edit === 'base' ? val : profiles.find((x) => x.id === edit)?.config || val;
    const config = JSON.parse(JSON.stringify({ detector: base.detector, stages: base.stages, track: base.track, max_fps: base.max_fps, run_when: base.run_when, motion_hold_s: base.motion_hold_s }));
    set({ profiles: [...profiles, { id, name, config, schedule: Array(168).fill(0) }] });
    setEdit(id);
  };
  const cur = sched ? profiles.find((x) => x.id === sched) : null;
  return (
    <Card title="Profiles" hint="Different pipelines for different times" actions={profiles.length < 4 && <Btn sm icon="plus" onClick={add}>Add profile</Btn>}>
      {!profiles.length && <div class="small dim">One pipeline runs all the time. Add a profile to run another pipeline at other times (for example ANPR by day and people detection at night), switched by a weekly schedule or by hand.</div>}
      {profiles.length > 0 && (
        <div class="col">
          <Field label="Running" help={st?.ai?.profile ? `Now: ${profiles.find((x) => x.id === st.ai.profile)?.name || st.ai.profile}` : ''}>
            <Select value={val.active_profile || 'auto'} onChange={(v) => set({ active_profile: v })} options={[['auto', 'By schedule'], ...profiles.map((x) => [x.id, x.name] as [string, string])]} />
          </Field>
          <div class="seg">
            <button type="button" class={edit === 'base' ? 'on' : ''} onClick={() => setEdit('base')}>Base pipeline</button>
            {profiles.map((x) => <button type="button" class={edit === x.id ? 'on' : ''} onClick={() => setEdit(x.id)}>{x.name}</button>)}
          </div>
          <div class="small dim">The cards on the left edit the {edit === 'base' ? 'base pipeline (used when no profile applies)' : 'selected profile'}.</div>
          {edit !== 'base' && (() => {
            const pr = profiles.find((x) => x.id === edit);
            return pr ? (
              <div class="row">
                <div class="grow"><Input value={pr.name} onInput={(v) => set({ profiles: profiles.map((x) => (x.id === edit ? { ...x, name: v } : x)) })} /></div>
                <Btn sm onClick={() => setSched(edit)}>Schedule</Btn>
                <Btn sm kind="ghost" icon="trash" onClick={async () => {
                  if (!(await confirmDialog('Delete profile', `Delete “${pr.name}”?`, 'Delete', true))) return;
                  set({ profiles: profiles.filter((x) => x.id !== edit), active_profile: val.active_profile === edit ? 'auto' : val.active_profile });
                  setEdit('base');
                }} />
              </div>
            ) : null;
          })()}
        </div>
      )}
      {cur && (
        <Modal wide title={`Schedule: ${cur.name}`} onClose={() => setSched(null)} footer={<Btn kind="primary" onClick={() => setSched(null)}>Done</Btn>}>
          <p class="muted small">The profile runs in the painted hours (when the Running selector is “By schedule”). Hours painted for none of the profiles fall back to the first profile.</p>
          <Schedule value={cur.schedule || []} onChange={(v) => set({ profiles: profiles.map((x) => (x.id === cur.id ? { ...x, schedule: v } : x)) })} />
        </Modal>
      )}
    </Card>
  );
}

function PipelineTab() {
  const { val, set, save, dirty, reload } = useSection<any>('ai');
  const { models } = useModels();
  const st = useStore(status);
  const [mm, setMm] = useState<any>(null);
  const [edit, setEdit] = useState('base');
  const [img, refresh] = useSnapshotUrl();
  useEffect(() => {
    api.get('/api/system/media-memory').then(setMm, () => {});
  }, []);
  if (!val) return <div class="empty"><span class="spin" /></div>;
  const profiles: any[] = val.profiles || [];
  const editing = edit !== 'base' && profiles.some((x) => x.id === edit) ? edit : 'base';
  // the cards edit either the base pipeline or one profile's pipeline
  const root = editing === 'base' ? val : profiles.find((x) => x.id === editing)?.config || {};
  const setRoot = (n: any) => (editing === 'base' ? set(n) : set({ profiles: profiles.map((x) => (x.id === editing ? { ...x, config: { ...x.config, ...n } } : x)) }));
  const det = root.detector || {};
  const detModels = models.filter((m) => m.task === 'detect' || m.task === 'face_detect');
  const labels = labelsOf(models, det.model);
  const isFace = models.find((m) => m.id === det.model)?.task === 'face_detect';
  const used = [det.model, ...(root.stages || []).flatMap((s: any) => [s.model, s.then?.model])].filter(Boolean);
  const estKb = used.reduce((a: number, id: string) => {
    const io = models.find((m) => m.id === id)?.meta?.io;
    return a + (io ? io.weights_kb + 200 : 1500);
  }, 0) + Math.max(0, ...used.map((id: string) => models.find((m) => m.id === id)?.meta?.io?.internal_kb || 0));
  const aiSt = st?.ai;

  const apply = async (t: (typeof TEMPLATES)[0]) => {
    if (!(await confirmDialog(`Use “${t.title}”?`, 'This replaces the current detector and stages' + (t.rules ? ' and adds its rules' : '') + (t.image ? `, and sets the “${t.image}” exposure preset` : '') + '. The video restarts for a moment.', 'Apply'))) return;
    try {
      await applyTemplate(t);
      toast('Template applied', 'ok');
      setTimeout(reload, 4000);
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  const tiles = det.tiles || [1, 1];

  return (
    <div class="col">
      <div class="grid g4">
        {TEMPLATES.map((t) => (
          <div class="card" style={{ padding: 16, cursor: 'pointer' }} role="button" tabIndex={0} onClick={() => apply(t)}>
            <div class="row" style={{ gap: 8 }}><Icon n={t.icon} /><b>{t.title}</b></div>
            <div class="small muted" style={{ marginTop: 6 }}>{t.desc}</div>
          </div>
        ))}
      </div>
      {aiSt?.error && <Callout kind="bad">{aiSt.error}</Callout>}
      <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) 320px', alignItems: 'start' }}>
        <div class="col">
          <Card title="Detector" hint="Runs on every analysed frame of the sub stream">
            <div class="form g2">
              <Field label="Model" class="span2">
                <Select value={det.model} onChange={(v) => setRoot(upd(root, 'detector', { ...det, model: v, classes: [] }))}
                  options={[['', '— choose a detection model —'], ...detModels.map((m) => [m.id, `${m.name}${m.source === 'uploaded' ? ' (uploaded)' : ''}`])] as any} />
              </Field>
              <Field label="Detect only" class="span2" help="Leave empty to report every class the model knows">
                <Chips all={labels} value={det.classes || []} onChange={(v) => setRoot(upd(root, 'detector.classes', v))} empty="pick a model" />
              </Field>
              <Field label="Confidence threshold"><Slider value={Math.round((det.conf ?? 0.4) * 100)} onChange={(v) => setRoot(upd(root, 'detector.conf', v / 100))} unit="%" /></Field>
              <Field label="Overlap (NMS)"><Slider value={Math.round((det.nms ?? 0.45) * 100)} onChange={(v) => setRoot(upd(root, 'detector.nms', v / 100))} unit="%" /></Field>
              <Field label="Ignore boxes smaller than" help="Pixels on the analysis frame"><Num value={det.min_box_px ?? 12} onChange={(v) => setRoot(upd(root, 'detector.min_box_px', v))} unit="px" /></Field>
              <Field label="Analysis rate (max)"><Slider value={root.max_fps ?? 10} min={1} max={25} onChange={(v) => setRoot({ max_fps: v })} unit=" fps" /></Field>
              <div class="span2">
                <ToggleRow title="Only analyse while something moves" desc="Runs the NPU only while pixel motion is seen (needs Motion detection on): less heat, fewer false alarms."
                  checked={root.run_when === 'motion'} onChange={(v) => setRoot({ run_when: v ? 'motion' : 'always' })} />
              </div>
              {root.run_when === 'motion' && <Field label="Keep analysing after motion stops"><Num value={root.motion_hold_s ?? 5} min={1} max={300} unit="s" onChange={(v) => setRoot({ motion_hold_s: v })} /></Field>}
            </div>
          </Card>
          {!isFace && (
            <Card title="Detection area" hint="Analyse only part of the picture: small objects get more of the model’s resolution">
              <div class="col">
                <ShapeEditor img={img} onRefresh={refresh} mode="polygon" value={det.roi || []} onChange={(v) => setRoot(upd(root, 'detector.roi', v))} />
                <div class="form g2">
                  <Field label="Tiles across" help="Split the area into overlapping tiles for very small objects. More tiles = slower."><Select value={String(tiles[0])} onChange={(v) => setRoot(upd(root, 'detector.tiles', [+v, tiles[1]]))} options={['1', '2', '3', '4']} /></Field>
                  <Field label="Tiles down"><Select value={String(tiles[1])} onChange={(v) => setRoot(upd(root, 'detector.tiles', [tiles[0], +v]))} options={['1', '2', '3', '4']} /></Field>
                </div>
                <div class="small dim">{(det.roi || []).length >= 3 ? 'Only this area is analysed; objects are kept when their feet / wheels are inside it.' : 'No area drawn: the whole picture is analysed.'}</div>
              </div>
            </Card>
          )}
          <Card title="Per-object stages" hint="Second-stage models on each tracked object" actions={
            <Btn sm icon="plus" onClick={() => setRoot({ stages: [...(root.stages || []), { name: 'attribute', type: 'classify', model: '', on: [], conf: 0.5, interval_ms: 500, votes: 3 }] })}>Add stage</Btn>
          }>
            <div class="col">
              {!(root.stages || []).length && <div class="dim small">No stages. Add one to read plates, classify vehicle types or colours, recognise faces, or send raw outputs to your own server…</div>}
              {(root.stages || []).map((s: any, i: number) => (
                <StageEditor s={s} models={models} parentLabels={labels}
                  onChange={(n) => setRoot({ stages: root.stages.map((x: any, j: number) => (j === i ? n : x)) })}
                  onRemove={() => setRoot({ stages: root.stages.filter((_: any, j: number) => j !== i) })} />
              ))}
            </div>
          </Card>
        </div>
        <div class="col">
          <Card title="Engine">
            <ToggleRow title="AI enabled" desc="Loads the models onto the NPU" checked={!!val.enabled} onChange={(v) => set({ enabled: v })} />
            <div class="section-title">Status</div>
            <dl class="kv">
              <dt>State</dt><dd>{aiSt?.running ? <Badge kind="ok">running</Badge> : <Badge>{aiSt?.enabled ? 'stopped' : 'off'}</Badge>}{aiSt?.gated && <> <Badge>motion-gated</Badge></>}</dd>
              <dt>Speed</dt><dd>{aiSt?.fps ?? 0} fps · {aiSt?.infer_ms ?? 0} ms/frame</dd>
              {aiSt?.npu && (aiSt.npu.inference_ms > 0 || aiSt.npu.preprocess_ms > 0) && (
                <><dt>Per model run</dt><dd title={aiSt.npu.note || 'Preparing the picture for the model (RGA = 2D hardware, no CPU time) and running it on the NPU'}>
                  {aiSt.npu.preprocess_ms} ms prepare ({aiSt.npu.preprocess}) · {aiSt.npu.inference_ms} ms NPU
                </dd></>
              )}
              <dt>Tracked now</dt><dd>{aiSt?.tracks ?? 0}</dd>
              {aiSt?.profile && <><dt>Profile</dt><dd>{(aiSt.profiles || []).find((x: any) => x.id === aiSt.profile)?.name || aiSt.profile}</dd></>}
            </dl>
            <div class="section-title">Media memory</div>
            <div class="small muted">
              This pipeline needs about <b>{(estKb / 1024).toFixed(1)} MB</b> of NPU memory.
              {mm && <> The board has <b>{mm.active_mb} MB</b> media memory in total ({Math.round(mm.used_kb / 1024)} MB in use).</>}
            </div>
            {mm && estKb / 1024 > 4.5 && mm.active_mb <= 24 && (
              <Callout kind="warn">Large pipelines need more media memory: set 28 MB (or 32 MB) in System › Performance, or lower the main-stream resolution.</Callout>
            )}
          </Card>
          <ProfilesCard val={val} set={set} edit={editing} setEdit={setEdit} />
        </div>
      </div>
      <SaveBar dirty={dirty} onSave={() => save()} onReset={reload} note="Changing models restarts the video for a moment; thresholds, classes and areas apply instantly" />
    </div>
  );
}

// ---------------------------------------------------------------- rules tab

const RULE_TYPES: [string, string][] = [
  ['intrusion', 'Intrusion — object enters a zone'],
  ['loitering', 'Loitering — stays in a zone too long'],
  ['line_cross', 'Line crossing / counting'],
  ['presence', 'Object detected (anywhere or in a zone)'],
  ['count', 'Crowd — too many objects in a zone'],
  ['track', 'Object record — one event per object (ANPR / face logs)'],
];

function RuleEditor(p: { rule: any; labels: string[]; attrs: string[]; others: any[]; onSave: (r: any) => void; onClose: () => void }) {
  const [r, setR] = useState<any>(JSON.parse(JSON.stringify(p.rule)));
  const [img, refresh] = useSnapshotUrl();
  const set = (k: string, v: any) => setR({ ...r, [k]: v });
  const needsLine = r.type === 'line_cross';
  const zoneOpt = r.type !== 'line_cross';
  return (
    <Modal wide title={p.rule.id ? 'Edit rule' : 'New rule'} onClose={p.onClose} footer={
      <>
        <Btn kind="ghost" onClick={p.onClose}>Cancel</Btn>
        <Btn kind="primary" icon="check" disabled={!r.name || (needsLine && r.line?.length !== 2)} onClick={() => p.onSave(r)}>Save rule</Btn>
      </>
    }>
      <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1.4fr) minmax(0,1fr)' }}>
        <div>
          {needsLine ? (
            <ShapeEditor img={img} onRefresh={refresh} mode="line" value={r.line || []} onChange={(v) => set('line', v)} extra={p.others} />
          ) : (
            <ShapeEditor img={img} onRefresh={refresh} mode="polygon" value={r.zone || []} onChange={(v) => set('zone', v)} extra={p.others} />
          )}
          {zoneOpt && <div class="small dim" style={{ marginTop: 6 }}>{(r.zone || []).length >= 3 ? 'Rule applies inside the zone (object feet / wheels).' : 'No zone drawn: the rule applies to the whole picture.'}</div>}
        </div>
        <div class="form">
          <Field label="Name"><Input value={r.name} onInput={(v) => set('name', v)} placeholder="e.g. Gate intrusion" /></Field>
          <Field label="Type"><Select value={r.type} onChange={(v) => set('type', v)} options={RULE_TYPES} /></Field>
          <Field label="Objects" help="Empty = any detected object"><Chips all={p.labels} value={r.classes || []} onChange={(v) => set('classes', v)} empty="choose a detector first" /></Field>
          {r.type === 'line_cross' && <Field label="Direction"><Select value={r.direction || 'any'} onChange={(v) => set('direction', v)} options={[['any', 'Both directions'], ['a_to_b', 'A → B only'], ['b_to_a', 'B → A only']]} /></Field>}
          {r.type === 'line_cross' && <ToggleRow title="Reset the counters at midnight" desc="Counters otherwise keep counting across days and restarts" checked={!!r.reset_daily} onChange={(v) => set('reset_daily', v)} />}
          {r.type === 'loitering' && <Field label="Alert after"><Num value={r.dwell_s ?? 10} onChange={(v) => set('dwell_s', v)} min={1} unit="s" /></Field>}
          {r.type === 'count' && <Field label="Alert when at least"><Num value={r.threshold ?? 3} onChange={(v) => set('threshold', v)} min={1} unit="objects" /></Field>}
          {r.type !== 'track' && <Field label="Cooldown" help="Minimum time between alerts of this rule"><Num value={r.cooldown_s ?? 10} onChange={(v) => set('cooldown_s', v)} min={0} unit="s" /></Field>}
          {r.type === 'track' && (
            <>
              <Field label="Merge repeats within" help="The same plate (or other value) seen again within this time counts as one visit. 0 = log every time."><Num value={r.dedupe_s ?? 0} onChange={(v) => set('dedupe_s', v)} min={0} unit="s" /></Field>
              {(r.dedupe_s ?? 0) > 0 && <Field label="Compare by" help="Attribute that identifies the object"><Select value={r.dedupe_attr || ''} onChange={(v) => set('dedupe_attr', v)} options={[['', 'First attribute read'], ...p.attrs.map((a) => [a, a] as [string, string])]} /></Field>}
            </>
          )}
          <Field label="Only when attribute…" help="e.g. plate_text in a watch-list (allow/deny lists), identity in a list of names">
            <div class="input-row">
              <Select value={r.attribute?.name || ''} onChange={(v) => set('attribute', v ? { ...(r.attribute || { op: 'exists' }), name: v } : undefined)}
                options={[['', '— no condition —'], ...[...new Set([...p.attrs, 'identity'])].map((a) => [a, a] as [string, string])]} />
              {r.attribute?.name && <Select value={r.attribute.op} onChange={(v) => set('attribute', { ...r.attribute, op: v })}
                options={[['exists', 'is read'], ['missing', 'is not read'], ['in', 'is in list'], ['not_in', 'is NOT in list']]} />}
            </div>
          </Field>
          {(r.attribute?.op === 'in' || r.attribute?.op === 'not_in') && (
            <>
              <Field label="List" help="One value per line">
                <textarea class="input" value={(r.attribute.values || []).join('\n')} onInput={(e) => set('attribute', { ...r.attribute, values: (e.target as HTMLTextAreaElement).value.split('\n').map((x) => x.trim()).filter(Boolean) })} />
              </Field>
              <Field label="Tolerate misread characters" help="A plate with this many wrong characters still matches the list (look-alikes such as 0/O or 8/B count for less)">
                <Select value={String(r.attribute.fuzzy ?? 0)} onChange={(v) => set('attribute', { ...r.attribute, fuzzy: +v })} options={[['0', 'Exact match'], ['1', 'Up to 1 character'], ['2', 'Up to 2 characters']]} />
              </Field>
            </>
          )}
          <ToggleRow title="Enabled" checked={r.enabled !== false} onChange={(v) => set('enabled', v)} />
        </div>
      </div>
    </Modal>
  );
}

function RulesTab() {
  const { val: rules, setVal, save, dirty, reload } = useSection<any[]>('rules');
  const { val: ai } = useSection<any>('ai');
  const { models } = useModels();
  const [edit, setEdit] = useState<any>(null);
  const [img, refresh] = useSnapshotUrl();
  if (!rules || !ai) return <div class="empty"><span class="spin" /></div>;
  const labels = ai.detector?.model ? labelsOf(models, ai.detector.model).filter((l) => !(ai.detector.classes?.length) || ai.detector.classes.includes(l)) : [];
  const attrs = [...new Set((ai.stages || []).flatMap((s: any) => [s.then ? s.then.name : s.name]).filter(Boolean))] as string[];
  const aiRules = rules.filter((r) => r.type !== 'motion');
  const saveRule = (r: any) => {
    const list = r.id ? rules.map((x) => (x.id === r.id ? r : x)) : [...rules, { ...r, id: Math.random().toString(16).slice(2, 10) }];
    setVal(list);
    setEdit(null);
  };
  return (
    <div class="col">
      {!ai.enabled && <Callout kind="warn">AI is off. Rules only run while the AI pipeline is enabled (Pipeline tab).</Callout>}
      <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) minmax(0,1.2fr)', alignItems: 'start' }}>
        <Card title="Rules" tight actions={<Btn sm kind="primary" icon="plus" onClick={() => setEdit({ name: '', type: 'intrusion', classes: [], cooldown_s: 10, enabled: true })}>Add rule</Btn>}>
          {!aiRules.length && <Empty icon="zone">No rules yet. Rules turn detections into events, alarms and webhooks.</Empty>}
          <table class="table">
            <tbody>
              {aiRules.map((r) => (
                <tr class="click" tabIndex={0} onClick={() => setEdit(r)}>
                  <td>
                    <div class="row" style={{ gap: 6 }}><b>{r.name}</b>{r.enabled === false && <Badge>off</Badge>}</div>
                    <div class="small dim">{RULE_TYPES.find((t) => t[0] === r.type)?.[1].split(' — ')[0]} · {(r.classes || []).join(', ') || 'any object'}{r.attribute?.name ? ` · ${r.attribute.name} ${r.attribute.op}` : ''}{r.dedupe_s ? ` · merge ${r.dedupe_s}s` : ''}</div>
                  </td>
                  <td class="right" onClick={(e) => e.stopPropagation()}>
                    <Switch checked={r.enabled !== false} onChange={(v) => setVal(rules.map((x) => (x === r ? { ...r, enabled: v } : x)))} />
                    <Btn sm kind="ghost" icon="trash" onClick={() => setVal(rules.filter((x) => x !== r))} />
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </Card>
        <Card title="Zones & lines" actions={<Btn sm kind="ghost" icon="refresh" onClick={refresh} />}>
          <div class="player">
            {img && <img src={img} />}
            <Overlay rules={aiRules} />
          </div>
        </Card>
      </div>
      <SaveBar dirty={dirty} onSave={() => save(true)} onReset={reload} />
      {edit && <RuleEditor rule={edit} labels={labels} attrs={attrs} others={aiRules.filter((x) => x !== edit)} onSave={saveRule} onClose={() => setEdit(null)} />}
    </div>
  );
}

// ---------------------------------------------------------------- models tab

function AdvancedModelFields(p: { task: string; meta: any; set: (k: string, v: any) => void }) {
  const m = p.meta;
  const [plugins, setPlugins] = useState<any[]>([]);
  useEffect(() => {
    if (isAdmin() && p.task === 'detect') api.get('/api/ai/plugins').then(setPlugins, () => {});
  }, [p.task]);
  return (
    <div class="form g2">
      {p.task === 'detect' && (
        <>
          <Field label="Output format" help="Automatic recognises the Rockchip YOLO exports. Pick one when your model is something else."><Select value={m.decoder || 'auto'} onChange={(v) => p.set('decoder', v)} options={DECODERS.filter((d) => d[0] !== 'plugin' || isAdmin())} /></Field>
          {(m.decoder === 'yolov5') && <Field label="Anchors" help="Custom-trained YOLOv5/v7 only: 18 numbers (3 heads × 3 anchors × w,h). Empty = COCO anchors."><Input mono value={Array.isArray(m.anchors) ? m.anchors.join(', ') : m.anchors || ''} placeholder="10,13,16,30,33,23,…" onInput={(v) => p.set('anchors', v.split(',').map((x) => +x.trim()).filter((x) => !isNaN(x) && x > 0))} /></Field>}
          {m.decoder === 'plugin' && <Field label="Decoder plugin"><Select value={m.plugin || ''} onChange={(v) => p.set('plugin', v)} options={[['', '— choose —'], ...plugins.map((x) => [x.file, x.file] as [string, string])]} /></Field>}
          <div class="span2"><ToggleRow title="One class per object" desc="Suppress overlapping boxes of different classes" checked={!!m.class_agnostic} onChange={(v) => p.set('class_agnostic', v)} /></div>
          <div class="span2"><ToggleRow title="Stretch instead of letterbox" desc="Resize the picture to the model input without grey padding (models trained that way)" checked={!!m.stretch} onChange={(v) => p.set('stretch', v)} /></div>
        </>
      )}
      {p.task === 'ocr' && (
        <>
          <Field label="Output layout" help="Classes × steps (PlateNet) or steps × classes (most CRNN exports)"><Select value={m.layout || 'ct'} onChange={(v) => p.set('layout', v)} options={[['ct', 'classes × steps'], ['tc', 'steps × classes']]} /></Field>
          <Field label="CTC blank class" help="Last (default) or first (PaddleOCR)"><Select value={String(m.blank || 'last')} onChange={(v) => p.set('blank', v)} options={[['last', 'Last'], ['first', 'First']]} /></Field>
        </>
      )}
      {p.task === 'classify' && <div class="span2"><ToggleRow title="Centre crop" desc="Use the central square of the object instead of stretching it" checked={!!m.center_crop} onChange={(v) => p.set('center_crop', v)} /></div>}
    </div>
  );
}

function UploadModel(p: { onDone: () => void; onClose: () => void }) {
  const [file, setFile] = useState<File | null>(null);
  const [name, setName] = useState('');
  const [task, setTask] = useState('');
  const [labels, setLabels] = useState('');
  const [color, setColor] = useState('rgb');
  const [charset, setCharset] = useState('');
  const [adv, setAdv] = useState<any>({});
  const [more, setMore] = useState(false);
  const [pct, setPct] = useState(-1);
  const [over, setOver] = useState(false);
  const pkg = !!file && /\.(tar\.gz|tgz|pcm)$/i.test(file.name);
  const go = async () => {
    if (!file) return;
    const q = new URLSearchParams({ filename: file.name, color });
    if (!pkg) {
      if (name) q.set('name', name);
      if (task) q.set('task', task);
      if (labels.trim()) q.set('labels', labels.split(/[\n,]/).map((x) => x.trim()).filter(Boolean).join(','));
      if (charset) q.set('charset', charset);
      for (const k of ['decoder', 'layout', 'blank', 'plugin']) if (adv[k]) q.set(k, adv[k]);
      for (const k of ['stretch', 'class_agnostic', 'center_crop']) if (adv[k]) q.set(k, 'true');
      if (Array.isArray(adv.anchors) && adv.anchors.length) q.set('anchors', adv.anchors.join(','));
    }
    try {
      setPct(0);
      const r = await uploadWithProgress(`/api/ai/models?${q}`, file, setPct);
      toast(`Installed ${r.id} (${TASK_SHORT[r.meta.task] || r.meta.task})`, 'ok');
      p.onDone();
      p.onClose();
    } catch (e: any) {
      toast(e.message, 'err');
      setPct(-1);
    }
  };
  return (
    <Modal title="Upload a model" onClose={p.onClose} footer={<><Btn kind="ghost" onClick={p.onClose}>Cancel</Btn><Btn kind="primary" icon="upload" disabled={!file || pct >= 0} onClick={go}>{pct >= 0 ? `${pct}%` : 'Upload'}</Btn></>}>
      <div class="form">
        <label class={'drop' + (over ? ' over' : '')} onDragOver={(e) => (e.preventDefault(), setOver(true))} onDragLeave={() => setOver(false)}
          onDrop={(e) => {
            e.preventDefault();
            setOver(false);
            const f = e.dataTransfer?.files?.[0];
            if (f) setFile(f);
          }}>
          <input type="file" accept=".rknn,.tar.gz,.tgz,.pcm" hidden onChange={(e) => setFile((e.target as HTMLInputElement).files?.[0] || null)} />
          <Icon n="upload" s={26} />
          <div>{file ? <><b>{file.name}</b> · {fmtBytes(file.size)}</> : 'Drop a .rknn file or a model package here'}</div>
          <div class="small dim">A <b>model package</b> (.tar.gz from <code>models/package.py</code>) carries the model, its labels and settings in one file. A plain .rknn: convert with RKNN-Toolkit2 for “rv1106” (INT8, uint8 NHWC input).</div>
        </label>
        {pkg ? <Callout>Everything is read from the package: name, task, labels and output format.</Callout> : (
          <>
            <div class="form g2">
              <Field label="Display name"><Input value={name} onInput={setName} placeholder="e.g. Helmet detector" /></Field>
              <Field label="Task"><Select value={task} onChange={setTask} options={[['', 'Detect automatically'], ...TASKS]} /></Field>
              <Field label="Input colour order" help="Most Ultralytics/YOLO exports are RGB"><Select value={color} onChange={setColor} options={[['rgb', 'RGB'], ['bgr', 'BGR']]} /></Field>
              {task === 'ocr' && <Field label="Character set" help="Model classes in order, without the blank"><Input mono value={charset} onInput={setCharset} placeholder="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" /></Field>}
            </div>
            <Field label="Class labels" help="One per line (or comma separated), in model class order">
              <textarea class="input" value={labels} onInput={(e) => setLabels((e.target as HTMLTextAreaElement).value)} placeholder={'helmet\nno_helmet'} />
            </Field>
            <a onClick={() => setMore(!more)} class="small">{more ? '▾' : '▸'} Advanced: output format, anchors, layout</a>
            {more && <AdvancedModelFields task={task || 'detect'} meta={adv} set={(k, v) => setAdv({ ...adv, [k]: v })} />}
          </>
        )}
      </div>
    </Modal>
  );
}

function ModelDetail(p: { m: any; onClose: () => void; onChanged: () => void }) {
  const m = p.m;
  const [meta, setMeta] = useState<any>({ ...m.meta });
  const [test, setTest] = useState<any>(null);
  const [bench, setBench] = useState<any>(null);
  const [img, refresh] = useSnapshotUrl();
  const io = m.meta?.io;
  const saveMeta = async () => {
    try {
      const body: any = {};
      for (const k of ['name', 'task', 'labels', 'color', 'charset', 'description', 'decoder', 'plugin', 'anchors', 'stretch', 'class_agnostic', 'layout', 'blank', 'center_crop']) if (meta[k] !== undefined) body[k] = meta[k];
      await api.put(`/api/ai/models/${m.id}`, body);
      toast('Saved', 'ok');
      p.onChanged();
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  const runTest = async () => {
    refresh();
    try {
      setTest(await api.post('/api/ai/test', { model: m.id, conf: 0.3 }));
    } catch (e: any) {
      toast(e.message, 'err');
    }
  };
  return (
    <Modal wide title={m.name} onClose={p.onClose} footer={
      <>
        {m.source === 'uploaded' && <Btn kind="danger" icon="trash" onClick={async () => {
          if (!(await confirmDialog('Delete model', `Delete ${m.id}?`, 'Delete', true))) return;
          await api.del(`/api/ai/models/${m.id}`).then(() => (toast('Deleted', 'ok'), p.onChanged(), p.onClose()), (e) => toast(e.message, 'err'));
        }}>Delete</Btn>}
        <span class="grow" />
        <Btn icon="bolt" onClick={async () => {
          try {
            setBench(await api.post('/api/ai/benchmark', { model: m.id }));
          } catch (e: any) {
            toast(e.message, 'err');
          }
        }}>Benchmark</Btn>
        <Btn icon="play" onClick={runTest}>Test on live picture</Btn>
        <Btn kind="primary" icon="check" onClick={saveMeta}>Save</Btn>
      </>
    }>
      <div class="grid g2">
        <div class="form">
          <Field label="Name"><Input value={meta.name} onInput={(v) => setMeta({ ...meta, name: v })} /></Field>
          <div class="form g2">
            <Field label="Task"><Select value={meta.task} onChange={(v) => setMeta({ ...meta, task: v })} options={TASKS} /></Field>
            <Field label="Colour order"><Select value={meta.color || 'rgb'} onChange={(v) => setMeta({ ...meta, color: v })} options={[['rgb', 'RGB'], ['bgr', 'BGR']]} /></Field>
          </div>
          {meta.task === 'ocr' && <Field label="Character set"><Input mono value={meta.charset || ''} onInput={(v) => setMeta({ ...meta, charset: v })} /></Field>}
          <AdvancedModelFields task={meta.task} meta={meta} set={(k, v) => setMeta({ ...meta, [k]: v })} />
          <Field label="Labels" help="One per line, in class order">
            <textarea class="input" style={{ minHeight: 120 }} value={(meta.labels || []).join('\n')} onInput={(e) => setMeta({ ...meta, labels: (e.target as HTMLTextAreaElement).value.split('\n').map((x) => x.trim()).filter(Boolean) })} />
          </Field>
          {io && (
            <dl class="kv">
              <dt>File</dt><dd class="mono">{m.id} · {fmtBytes(m.size)}</dd>
              <dt>Head</dt><dd>{io.head || io.task_guess} {io.classes_guess > 0 && `· ${io.classes_guess} classes`}</dd>
              <dt>Input</dt><dd class="mono">{io.inputs?.map((x: any) => `${x.dims} ${x.type}`).join(', ')}</dd>
              <dt>Outputs</dt><dd class="mono">{io.outputs?.map((x: any) => x.dims).join(' · ')}</dd>
              <dt>NPU memory</dt><dd>{io.weights_kb} KB weights + {io.internal_kb} KB working</dd>
              <dt>Toolkit</dt><dd>{io.toolkit}</dd>
            </dl>
          )}
        </div>
        <div class="col">
          <div class="player">
            {img && <img src={img} />}
            {test && <Overlay dets={(test.results || []).filter((r: any) => r.box).map((r: any, i: number) => ({ id: i + 1, label: r.label, score: r.score, box: r.box, attrs: { s: `${Math.round(r.score * 100)}%` } }))} />}
          </div>
          {bench && <Callout kind="ok">{bench.avg_ms} ms per run on this NPU (best {bench.best_ms} ms) → up to <b>{bench.max_fps} fps</b> for this model alone · input {bench.input}</Callout>}
          {test && (
            <div class="small muted">
              {test.results.length} result(s) in {test.ms} ms
              {test.results.filter((r: any) => !r.box).map((r: any) => <div><b>{r.label}</b> · {Math.round(r.score * 100)}%</div>)}
            </div>
          )}
          <div class="small dim">Testing and benchmarking load the model temporarily. If the camera runs a large pipeline, stop the AI first to free NPU memory.</div>
        </div>
      </div>
    </Modal>
  );
}

function PluginsCard() {
  const [plugins, setPlugins] = useState<any[]>([]);
  const load = () => api.get('/api/ai/plugins').then(setPlugins, () => {});
  useEffect(() => {
    load();
  }, []);
  return (
    <Card title="Decoder plugins" hint="For models with an output format the camera does not know" tight actions={
      <label class="btn sm"><Icon n="upload" s={15} /> Upload .so
        <input type="file" accept=".so" hidden onChange={async (e) => {
          const f = (e.target as HTMLInputElement).files?.[0];
          if (!f) return;
          try {
            setPlugins(await uploadWithProgress(`/api/ai/plugins?filename=${encodeURIComponent(f.name)}`, f, () => {}));
            toast('Plugin installed', 'ok');
          } catch (x: any) {
            toast(x.message, 'err');
          }
        }} />
      </label>
    }>
      <div class="small dim" style={{ padding: '10px 14px' }}>A plugin is a small shared library that turns the model’s output tensors into boxes. It runs inside the camera program, so only administrators can add one. See <code>sdk/example_decoder.c</code> in the project.</div>
      {plugins.length > 0 && <table class="table"><tbody>
        {plugins.map((x) => <tr><td class="mono">{x.file}</td><td class="nowrap">{fmtBytes(x.size)}</td><td class="right"><Btn sm kind="ghost" icon="trash" onClick={async () => setPlugins(await api.del(`/api/ai/plugins/${x.file}`))} /></td></tr>)}
      </tbody></table>}
    </Card>
  );
}

function ModelsTab() {
  const { models, reload } = useModels();
  const [up, setUp] = useState(false);
  const [sel, setSel] = useState<any>(null);
  return (
    <div class="col">
      <Callout>
        Bring your own models: train with Ultralytics (YOLOv5/v7/v8/v10/11, YOLOX), PyTorch or Paddle, export to ONNX and convert with RKNN-Toolkit2 for
        <b> rv1106</b> (INT8). Detectors, classifiers, CTC text readers and face models are understood directly; for anything else use a decoder plugin or a <b>raw</b> stage that
        sends the output tensors to your server. Bundle model + labels + settings with <code>models/package.py</code>.
      </Callout>
      <Card title="Model library" tight actions={<Btn sm kind="primary" icon="upload" onClick={() => setUp(true)}>Upload model</Btn>}>
        {!models.length && <Empty icon="box">No models</Empty>}
        <table class="table">
          <thead><tr><th>Name</th><th>Task</th><th>Classes</th><th>Size</th><th>Source</th></tr></thead>
          <tbody>
            {models.map((m) => (
              <tr class="click" tabIndex={0} onClick={() => setSel(m)}>
                <td><b>{m.name}</b><div class="mono dim">{m.id}</div></td>
                <td><Badge kind="accent">{TASK_SHORT[m.task] || m.task || '?'}</Badge>{m.meta?.decoder && m.meta.decoder !== 'auto' && <> <Badge>{m.meta.decoder}</Badge></>}</td>
                <td class="small muted">{m.meta?.labels?.length ? `${m.meta.labels.length}: ${m.meta.labels.slice(0, 4).join(', ')}${m.meta.labels.length > 4 ? '…' : ''}` : '–'}</td>
                <td class="nowrap">{fmtBytes(m.size)}</td>
                <td><Badge>{m.source}</Badge></td>
              </tr>
            ))}
          </tbody>
        </table>
      </Card>
      {isAdmin() && <PluginsCard />}
      {up && <UploadModel onDone={reload} onClose={() => setUp(false)} />}
      {sel && <ModelDetail m={sel} onClose={() => setSel(null)} onChanged={reload} />}
    </div>
  );
}

// ---------------------------------------------------------------- faces tab

function FacesTab() {
  const [d, setD] = useState<any>(null);
  const [name, setName] = useState('');
  const [shots, setShots] = useState(3);
  const load = () => api.get('/api/faces').then(setD, () => {});
  useEffect(() => {
    load();
    const t = setInterval(load, 1500);
    return () => clearInterval(t);
  }, []);
  const en = d?.enroll;
  return (
    <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) minmax(0,1fr)', alignItems: 'start' }}>
      <Card title="Enrol a person" hint="The camera learns a face from its own view">
        <div class="form">
          <Callout>Needs the “Face capture & recognition” pipeline (AI on, face embedding stage). Stand in front of the camera, look at it and move your head slightly while it collects the shots.</Callout>
          <div class="form g2">
            <Field label="Name"><Input value={name} onInput={setName} placeholder="e.g. Anita" /></Field>
            <Field label="Shots" help="More shots = more reliable"><Num value={shots} min={1} max={8} onChange={setShots} /></Field>
          </div>
          {en?.active ? (
            <div class="row">
              <Badge kind="warn">waiting for a face… {en.got}/{en.shots}</Badge>
              <span class="grow" />
              <Btn onClick={async () => setD({ ...d, enroll: await api.post('/api/faces/cancel') })}>Cancel</Btn>
            </div>
          ) : (
            <Btn kind="primary" icon="user" disabled={!name.trim()} onClick={async () => {
              try {
                setD({ ...d, enroll: await api.post('/api/faces/enroll', { name: name.trim(), shots }) });
                setName('');
              } catch (e: any) {
                toast(e.message, 'err');
              }
            }}>Start enrolment</Btn>
          )}
        </div>
      </Card>
      <Card title="Enrolled people" tight>
        {!d?.people?.length && <Empty icon="user">Nobody enrolled yet</Empty>}
        <table class="table"><tbody>
          {(d?.people || []).map((x: any) => (
            <tr>
              <td><b>{x.name}</b><div class="dim small">{x.shots} shots</div></td>
              <td class="right">
                <Btn sm kind="ghost" icon="trash" onClick={async () => {
                  if (!(await confirmDialog('Remove person', `Remove ${x.name}?`, 'Remove', true))) return;
                  api.del(`/api/faces/${x.id}`).then((l) => setD({ ...d, people: l }), (e) => toast(e.message, 'err'));
                }} />
              </td>
            </tr>
          ))}
        </tbody></table>
        <div class="small dim" style={{ padding: '10px 14px' }}>Add a rule with “identity is in list” (Rules tab) to alert or log specific people, or “is NOT in list” for strangers.</div>
      </Card>
    </div>
  );
}

// ---------------------------------------------------------------- motion tab

function MotionTab() {
  const { val, set, save, dirty, reload } = useSection<any>('motion');
  const [img, refresh] = useSnapshotUrl();
  const st = useStore(status);
  if (!val) return <div class="empty"><span class="spin" /></div>;
  return (
    <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1.3fr) minmax(0,1fr)', alignItems: 'start' }}>
      <Card title="Detection area" hint="No zone = whole picture">
        <ShapeEditor img={img} onRefresh={refresh} mode="polygon" value={val.zones?.[0] || []} onChange={(v) => set({ zones: v.length ? [v] : [] })} />
      </Card>
      <Card title="Motion detection" hint="Pixel-based, no NPU needed">
        <div class="form">
          <ToggleRow title="Enabled" desc="Creates Motion events (and triggers event recording)" checked={!!val.enabled} onChange={(v) => set({ enabled: v })} />
          <Field label="Sensitivity"><Slider value={val.sensitivity ?? 60} onChange={(v) => set({ sensitivity: v })} /></Field>
          <Field label="Minimum moving area" help="Share of the area that must change"><Slider value={val.min_area_pct ?? 1} min={0.2} max={20} step={0.2} onChange={(v) => set({ min_area_pct: v })} unit="%" /></Field>
          <Field label="Cooldown"><Num value={val.cooldown_s ?? 10} onChange={(v) => set({ cooldown_s: v })} min={1} unit="s" /></Field>
          {st?.motion?.enabled && <div class="small muted">Current activity: <b>{st.motion.level_pct}%</b></div>}
        </div>
      </Card>
      <div class="span2"><SaveBar dirty={dirty} onSave={() => save()} onReset={reload} /></div>
    </div>
  );
}

export function AiPage() {
  const [tab, setTab] = useState(localStorage.getItem('pc-ai-tab') || 'pipeline');
  const pick = (t: string) => {
    setTab(t);
    localStorage.setItem('pc-ai-tab', t);
  };
  return (
    <div>
      <Tabs value={tab} onChange={pick} tabs={[['pipeline', 'Pipeline'], ['rules', 'Rules & zones'], ['models', 'Models'], ['faces', 'Faces'], ['motion', 'Motion detection']]} />
      {tab === 'pipeline' && <PipelineTab />}
      {tab === 'rules' && <RulesTab />}
      {tab === 'models' && <ModelsTab />}
      {tab === 'faces' && <FacesTab />}
      {tab === 'motion' && <MotionTab />}
    </div>
  );
}
