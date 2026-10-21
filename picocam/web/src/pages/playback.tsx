import { useEffect, useRef, useState } from 'preact/hooks';
import { api, fmtBytes } from '../api';
import { canEdit } from '../store';
import { Icon } from '../ui/icons';
import { Badge, Btn, Card, confirmDialog, Empty, Select, toast } from '../ui/kit';

const sec = (hhmmss: string) => +hhmmss.slice(0, 2) * 3600 + +hhmmss.slice(2, 4) * 60 + +hhmmss.slice(4, 6);
const hms = (s: number) => `${String(Math.floor(s / 3600)).padStart(2, '0')}:${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

export function PlaybackPage() {
  const [days, setDays] = useState<string[]>([]);
  const [day, setDay] = useState('');
  const [files, setFiles] = useState<any[]>([]);
  const [cur, setCur] = useState<any>(null);
  const [cursor, setCursor] = useState<number | null>(null);
  const [storage, setStorage] = useState<any>(null);
  const video = useRef<HTMLVideoElement>(null);
  const filesRef = useRef<any[]>([]);
  filesRef.current = files;
  // deep link from an event: #/playback?date=YYYY-MM-DD&t=HHMMSS
  const link = new URLSearchParams(location.hash.split('?')[1] || '');
  const jumped = useRef(false);

  useEffect(() => {
    api.get('/api/storage').then(setStorage, () => {});
    api.get('/api/recordings/days').then((d: string[]) => {
      setDays(d);
      const want = link.get('date');
      setDay(want && d.includes(want) ? want : d[d.length - 1] || '');
    });
  }, []);
  const loadFiles = () => day && api.get(`/api/recordings?date=${day}`).then((l: any[]) => {
    setFiles(l);
    const t = link.get('t');
    if (t && !jumped.current && link.get('date') === day) {
      jumped.current = true;
      const tt = sec(t);
      const hit = l.find((f) => sec(f.start) <= tt && tt <= (f.end ? sec(f.end) : sec(f.start) + 600)) || l.filter((f) => sec(f.start) <= tt).pop();
      if (hit) play(hit, Math.max(0, tt - sec(hit.start) - 3));
    }
  });
  useEffect(() => {
    setCur(null);
    loadFiles();
  }, [day]);

  // segments on the 24 h bar
  const segs = files.map((f) => {
    const s = sec(f.start);
    let e = f.end ? sec(f.end) : s + 60;
    if (f.in_progress) {
      const n = new Date();
      e = n.getHours() * 3600 + n.getMinutes() * 60 + n.getSeconds();
    }
    if (e < s) e = 86400;
    return { f, s, e };
  });
  const play = (f: any, offset = 0) => {
    setCur(f);
    setTimeout(() => {
      const v = video.current;
      if (!v) return;
      v.src = `/api/recordings/file/${day}/${f.name}`;
      // continuous playback: when a file ends, the next one of the same stream starts
      v.onended = () => {
        const next = filesRef.current.filter((x) => x.stream === f.stream && x.start > f.start && !x.in_progress)[0];
        if (next) play(next);
      };
      v.onloadedmetadata = () => {
        if (offset > 0) v.currentTime = offset;
        v.play().catch(() => {});
      };
    }, 0);
  };
  const clickBar = (ev: MouseEvent) => {
    const r = (ev.currentTarget as HTMLElement).getBoundingClientRect();
    const t = ((ev.clientX - r.left) / r.width) * 86400;
    const hit = segs.find((x) => t >= x.s && t <= x.e) || segs.reduce<any>((best, x) => (!best || Math.abs(x.s - t) < Math.abs(best.s - t) ? x : best), null);
    if (hit) play(hit.f, Math.max(0, t - hit.s));
  };
  useEffect(() => {
    const v = video.current;
    if (!v || !cur) return;
    const f = () => setCursor(sec(cur.start) + v.currentTime);
    v.addEventListener('timeupdate', f);
    return () => v.removeEventListener('timeupdate', f);
  }, [cur]);

  if (storage && !storage.available)
    return <Card><Empty icon="storage">No storage available. Insert an SD card (Pico Plus) or check Storage & Recording.</Empty></Card>;

  return (
    <div class="col">
      <div class="row">
        <div style={{ width: 200 }}>
          <Select value={day} onChange={setDay} options={days.length ? [...days].reverse() : [['', 'No recordings']] as any} />
        </div>
        <Btn icon="refresh" kind="ghost" onClick={loadFiles}>Refresh</Btn>
        <span class="grow" />
        {storage && <span class="muted small">{storage.mode !== 'off' ? <Badge kind="ok">Recording: {storage.mode}</Badge> : <Badge>Recording off</Badge>} &nbsp; {fmtBytes(storage.free_bytes)} free</span>}
      </div>
      <div class="timeline" onClick={clickBar}>
        {segs.map((x) => <div class={'seg-rec' + (x.f.event ? ' ev' : '')} style={{ left: `${(x.s / 86400) * 100}%`, width: `${Math.max(0.15, ((x.e - x.s) / 86400) * 100)}%` }} title={x.f.name} />)}
        {[0, 3, 6, 9, 12, 15, 18, 21, 24].map((h) => <span class="tick" style={{ left: `${(h / 24) * 100}%` }}>{h}:00</span>)}
        {cursor !== null && <div class="cursor" style={{ left: `${(cursor / 86400) * 100}%` }} />}
      </div>
      <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) 340px', alignItems: 'start' }}>
        <div class="player">
          {cur ? <video ref={video} controls playsInline /> : <div class="msg"><div><Icon n="playback" s={30} /><div class="small">Pick a time on the bar or a clip from the list</div></div></div>}
        </div>
        <Card title={`Clips · ${day || '–'}`} tight>
          <div style={{ maxHeight: 520, overflowY: 'auto' }}>
            {!files.length && <Empty icon="playback">No recordings on this day</Empty>}
            <table class="table">
              <tbody>
                {[...files].reverse().map((f) => (
                  <tr class={'click'} onClick={() => play(f)} style={cur?.name === f.name ? { background: 'var(--accent-soft)' } : {}}>
                    <td>
                      <div class="row" style={{ gap: 6 }}>
                        <b>{hms(sec(f.start))}</b>
                        {f.end && <span class="dim small">– {hms(sec(f.end))}</span>}
                        {f.in_progress && <Badge kind="bad">REC</Badge>}
                        {f.event && <Badge kind="warn">event</Badge>}
                      </div>
                      <div class="dim small">{f.stream} · {fmtBytes(f.size)}</div>
                    </td>
                    <td class="right nowrap" onClick={(e) => e.stopPropagation()}>
                      <a class="btn sm icon ghost" title="Download" href={`/api/recordings/file/${day}/${f.name}?download=1`}><Icon n="download" s={15} /></a>
                      {canEdit() && !f.in_progress && (
                        <Btn sm kind="ghost" icon="trash" title="Delete" onClick={async () => {
                          if (!(await confirmDialog('Delete recording', `Delete ${f.name}?`, 'Delete', true))) return;
                          await api.del(`/api/recordings/file/${day}/${f.name}`).then(() => toast('Deleted', 'ok'), (e) => toast(e.message, 'err'));
                          loadFiles();
                        }} />
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Card>
      </div>
    </div>
  );
}
