import { useState } from 'preact/hooks';
import { api } from '../api';
import { Btn, Callout, Field, Input, Modal, Seg, Select, toast } from '../ui/kit';
import { Icon } from '../ui/icons';
import { applyTemplate, TEMPLATES } from './ai';
import { ZONES } from './network';

const SCENES: [string, string][] = [['general', 'General'], ['anpr', 'Road / number plates'], ['indoor', 'Indoor'], ['lowlight', 'Low light']];

// First-run guide, shown once after the camera is activated
export function Wizard(p: { onClose: () => void }) {
  const [step, setStep] = useState(0);
  const [name, setName] = useState('');
  const [loc, setLoc] = useState('');
  const [tz, setTz] = useState('Asia/Kolkata');
  const [scene, setScene] = useState('general');
  const [ai, setAi] = useState('');
  const [rec, setRec] = useState('events');
  const [busy, setBusy] = useState(false);
  const steps = ['Name & time', 'Picture', 'AI', 'Recording', 'Done'];

  const finish = async () => {
    setBusy(true);
    try {
      if (name || loc) await api.put('/api/config/device', { ...(name ? { name } : {}), location: loc });
      const z = ZONES.find((q) => q[0] === tz);
      if (z) await api.put('/api/config/time', { tz_name: z[0], timezone: z[1] });
      await api.put('/api/config/image', { exposure: { preset: scene } });
      const t = TEMPLATES.find((x) => x.id === ai);
      if (t) await applyTemplate({ ...t, image: undefined });
      await api.put('/api/config/recording', { mode: rec });
      localStorage.setItem('pc-wizard', 'done');
      setStep(4);
    } catch (e: any) {
      toast(e.message, 'err');
    } finally {
      setBusy(false);
    }
  };
  const close = () => {
    localStorage.setItem('pc-wizard', 'done');
    p.onClose();
  };
  return (
    <Modal title={<><Icon n="camera" s={18} /> Set up your camera</>} onClose={close} footer={
      <>
        {step < 4 && <Btn kind="ghost" onClick={close}>Skip</Btn>}
        <span class="grow" />
        {step > 0 && step < 4 && <Btn onClick={() => setStep(step - 1)}>Back</Btn>}
        {step < 3 && <Btn kind="primary" onClick={() => setStep(step + 1)}>Next</Btn>}
        {step === 3 && <Btn kind="primary" icon="check" disabled={busy} onClick={finish}>Apply</Btn>}
        {step === 4 && <Btn kind="primary" onClick={p.onClose}>Open the camera</Btn>}
      </>
    }>
      <div class="row small dim" style={{ marginBottom: 14, flexWrap: 'wrap' }}>
        {steps.map((s, i) => <span style={{ fontWeight: i === step ? 700 : 400, color: i === step ? 'var(--accent)' : undefined }}>{i + 1}. {s}</span>)}
      </div>
      {step === 0 && (
        <div class="form">
          <Field label="Camera name" help="Shown on the picture, in ONVIF and in alerts"><Input value={name} onInput={setName} placeholder="e.g. Main gate" /></Field>
          <Field label="Location"><Input value={loc} onInput={setLoc} placeholder="optional" /></Field>
          <Field label="Time zone"><Select value={tz} onChange={setTz} options={ZONES.map((z) => [z[0], z[2]]) as any} /></Field>
        </div>
      )}
      {step === 1 && (
        <div class="form">
          <p class="muted small">What does the camera watch? This sets exposure, noise reduction and dynamic range to suit the scene. You can fine-tune it later under Image.</p>
          <Seg value={scene} onChange={setScene} options={SCENES} />
        </div>
      )}
      {step === 2 && (
        <div class="form">
          <p class="muted small">Pick what the AI should do. Models run on the camera’s NPU.</p>
          <div class="grid g2">
            {[{ id: '', title: 'No AI for now', desc: 'Live view, recording and motion only.' }, ...TEMPLATES].map((t: any) => (
              <div class="card" role="button" tabIndex={0} style={{ padding: 12, cursor: 'pointer', borderColor: ai === t.id ? 'var(--accent)' : undefined }} onClick={() => setAi(t.id)}>
                <b>{t.title}</b>
                <div class="small muted" style={{ marginTop: 4 }}>{t.desc}</div>
              </div>
            ))}
          </div>
        </div>
      )}
      {step === 3 && (
        <div class="form">
          <p class="muted small">Recording needs an SD card (Storage page shows its state).</p>
          <Seg value={rec} onChange={setRec} options={[['off', 'Off'], ['events', 'On events'], ['continuous', 'Continuous']]} />
          <Callout>Everything here can be changed later. Applying restarts the video for a moment.</Callout>
        </div>
      )}
      {step === 4 && (
        <div class="form">
          <Callout kind="ok">The camera is ready.</Callout>
          <dl class="kv">
            <dt>Live stream</dt><dd class="mono">rtsp://{location.hostname}/live/main</dd>
            <dt>ONVIF</dt><dd class="mono">http://{location.host}/onvif/device_service</dd>
            <dt>Snapshot</dt><dd class="mono">http://{location.host}/snapshot.jpg</dd>
          </dl>
        </div>
      )}
    </Modal>
  );
}
