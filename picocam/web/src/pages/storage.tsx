import { useEffect, useRef, useState } from 'preact/hooks';
import { api, fmtBytes } from '../api';
import { isAdmin } from '../store';
import { Badge, Btn, Callout, Card, confirmDialog, Field, Num, SaveBar, Seg, Select, Stat, toast, ToggleRow, useSection } from '../ui/kit';

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

export function Schedule(p: { value: number[]; onChange: (v: number[]) => void }) {
  const v = p.value?.length === 168 ? p.value : Array(168).fill(1);
  const painting = useRef<number | null>(null);
  const paint = (i: number) => {
    if (painting.current === null) return;
    const n = [...v];
    n[i] = painting.current;
    p.onChange(n);
  };
  useEffect(() => {
    const up = () => (painting.current = null);
    addEventListener('pointerup', up);
    addEventListener('pointercancel', up);
    return () => {
      removeEventListener('pointerup', up);
      removeEventListener('pointercancel', up);
    };
  }, []);
  return (
    <div>
      <div class="sched" style={{ touchAction: 'none' }} onPointerMove={(e) => {
        // touch pointers stay captured by the first cell: find the cell under the finger instead
        const el = document.elementFromPoint(e.clientX, e.clientY) as HTMLElement | null;
        const i = el?.dataset?.i;
        if (i !== undefined) paint(+i);
      }}>
        <span />
        {Array.from({ length: 24 }, (_, h) => <span>{h % 3 === 0 ? h : ''}</span>)}
        {DAYS.map((d, di) => (
          <>
            <span>{d}</span>
            {Array.from({ length: 24 }, (_, h) => {
              const i = di * 24 + h;
              return <i data-i={i} class={v[i] ? 'on' : ''} onPointerDown={(e) => {
                (e.target as HTMLElement).releasePointerCapture?.(e.pointerId);
                painting.current = v[i] ? 0 : 1;
                paint(i);
              }} />;
            })}
          </>
        ))}
      </div>
      <div class="row small" style={{ marginTop: 8 }}>
        <span class="dim grow">Click or drag to paint recording hours.</span>
        <Btn sm kind="ghost" onClick={() => p.onChange(Array(168).fill(1))}>All</Btn>
        <Btn sm kind="ghost" onClick={() => p.onChange(Array(168).fill(0))}>None</Btn>
        <Btn sm kind="ghost" onClick={() => p.onChange(Array.from({ length: 168 }, (_, i) => (i % 24 >= 19 || i % 24 < 7 ? 1 : 0)))}>Nights</Btn>
      </div>
    </div>
  );
}

export function StoragePage() {
  const rec = useSection<any>('recording');
  const ev = useSection<any>('events');
  const [st, setSt] = useState<any>(null);
  const [flashLimited, setFlashLimited] = useState(false);
  useEffect(() => {
    api.get('/api/status').then((s) => setFlashLimited(!!s.device?.board?.flash_limited), () => {});
  }, []);
  const [fs, setFs] = useState('vfat');
  const load = () => api.get('/api/storage').then(setSt, () => {});
  useEffect(() => {
    load();
    const t = setInterval(load, 5000);
    return () => clearInterval(t);
  }, []);
  const r = rec.val;
  if (!r || !ev.val) return <div class="empty"><span class="spin" /></div>;
  const used = st ? st.total_bytes - st.free_bytes : 0;
  return (
    <div class="col">
      <div class="grid g4">
        <Stat icon="storage" k="Storage" v={st?.available ? fmtBytes(st.free_bytes) : 'none'} unit={st?.available ? `free of ${fmtBytes(st.total_bytes)}` : ''} pct={st?.total_bytes ? (used / st.total_bytes) * 100 : undefined} />
        <Stat icon="rec" k="Recording" v={st?.recording ? 'Active' : 'Idle'} unit={st?.mode} />
        <Stat icon="video" k="Current file" v={st?.current_file ? fmtBytes(st.current_bytes) : '–'} unit={st?.current_file || ''} />
        <Stat icon="info" k="Medium" v={st?.kind === 'boot_sd' ? 'SD (system)' : st?.kind === 'sd' ? 'SD card' : st?.kind === 'sd_unmounted' ? 'SD (unreadable)' : st?.kind === 'sd_ejected' ? 'SD (removed safely)' : 'Internal'} unit={st?.fs || ''} />
      </div>
      {st?.error && <Callout kind="bad">{st.error}</Callout>}
      {st?.dropped_frames > 0 && <Callout kind="warn">{st.dropped_frames} frames were dropped because the card was too slow. Use a Class 10 / U1 card or record the sub stream.</Callout>}
      <div class="grid g2">
        <Card title="Recording">
          <div class="form">
            <Field label="Mode">
              <Seg value={r.mode} onChange={(x) => rec.set({ mode: x })} options={[['off', 'Off'], ['continuous', 'Continuous'], ['schedule', 'Schedule'], ['events', 'On events']]} />
            </Field>
            <div class="form g2">
              <Field label="Stream"><Select value={r.stream} onChange={(x) => rec.set({ stream: x })} options={[['main', 'Main (full quality)'], ['sub', 'Sub (saves space)']]} /></Field>
              <Field label="File length"><Num value={r.segment_min} min={1} max={60} onChange={(x) => rec.set({ segment_min: x })} unit="min" /></Field>
              {r.mode === 'events' && (
                <>
                  <Field label="Pre-event" help="Kept in memory, max ~2.5 MB"><Num value={r.pre_s} min={0} max={10} onChange={(x) => rec.set({ pre_s: x })} unit="s" /></Field>
                  <Field label="Post-event"><Num value={r.post_s} min={1} max={600} onChange={(x) => rec.set({ post_s: x })} unit="s" /></Field>
                </>
              )}
              <Field label="Keep free" help="Space reserved on the card"><Num value={r.reserve_mb} min={64} max={100000} onChange={(x) => rec.set({ reserve_mb: x })} unit="MB" /></Field>
            </div>
            <ToggleRow title="Overwrite oldest" desc="Loop recording: delete the oldest files when the card is full" checked={r.overwrite !== false} onChange={(x) => rec.set({ overwrite: x })} />
            {r.mode === 'schedule' && <Schedule value={r.schedule} onChange={(x) => rec.set({ schedule: x })} />}
          </div>
          <SaveBar dirty={rec.dirty} onSave={() => rec.save()} onReset={rec.reload} />
        </Card>
        <div class="col">
          <Card title="Event pictures">
            <div class="form">
              <ToggleRow title="Full-resolution snapshot with each event" checked={ev.val.snapshot !== false} onChange={(x) => ev.set({ snapshot: x })} />
              <Field label="Event picture from" help="Main stream: full resolution, taken a moment after the detection. AI frame: lower resolution but exactly the moment of the detection (better for fast vehicles).">
                <Seg value={ev.val.snapshot_source || 'main'} onChange={(x) => ev.set({ snapshot_source: x })} options={[['main', 'Main stream'], ['ai', 'AI frame']]} />
              </Field>
              <ToggleRow title="Store events on the SD card" desc="Otherwise internal memory (limited to a few hundred events)" checked={ev.val.store_on_sd !== false} onChange={(x) => ev.set({ store_on_sd: x })} />
              {flashLimited && (
                <ToggleRow title="Keep event pictures in internal flash when there is no SD card" desc="This board has SPI NAND: every picture is a flash write. Off = pictures live in RAM and are lost on reboot (recommended without an SD card). Insert an SD card for permanent storage." checked={!!ev.val.internal_flash} onChange={(x) => ev.set({ internal_flash: x })} />
              )}
              <Field label="Keep at most"><Num value={ev.val.keep} min={50} max={100000} onChange={(x) => ev.set({ keep: x })} unit="events" /></Field>
            </div>
            <SaveBar dirty={ev.dirty} onSave={() => ev.save()} onReset={ev.reload} />
          </Card>
          <Card title="SD card">
            <dl class="kv">
              <dt>Device</dt><dd>{st?.device || '–'}</dd>
              <dt>Path</dt><dd class="mono">{st?.path || '–'}</dd>
              <dt>Filesystem</dt><dd>{st?.fs || '–'}</dd>
            </dl>
            {st?.kind === 'boot_sd' && <div class="small dim mt">This board boots from the SD card; recordings use its free space. Formatting is disabled to protect the system.</div>}
            {(st?.can_eject || st?.kind === 'sd_ejected') && (
              <div class="row mt">
                {st.can_eject ? (
                  <Btn icon="eject" onClick={async () => {
                    try {
                      setSt(await api.post('/api/storage/eject'));
                      toast('You can remove the SD card now', 'ok');
                    } catch (e: any) {
                      toast(e.message, 'err');
                    }
                  }}>Safely remove card</Btn>
                ) : (
                  <Btn onClick={async () => setSt(await api.post('/api/storage/mount'))}>Mount card again</Btn>
                )}
              </div>
            )}
            {st?.can_format && isAdmin() && (
              <div class="row mt">
                <Seg value={fs} onChange={setFs} options={[['vfat', 'FAT32 (PC-readable)'], ['ext4', 'ext4 (robust)']]} />
                <Btn kind="danger" onClick={async () => {
                  if (!(await confirmDialog('Format SD card', 'All data on the SD card will be erased.', 'Format', true))) return;
                  try {
                    setSt(await api.post('/api/storage/format', { fs }));
                    toast('SD card formatted', 'ok');
                  } catch (e: any) {
                    toast(e.message, 'err');
                  }
                }}>Format</Btn>
              </div>
            )}
            {st && !st.available && <div class="mt"><Badge kind="warn">no storage</Badge></div>}
          </Card>
        </div>
      </div>
    </div>
  );
}
