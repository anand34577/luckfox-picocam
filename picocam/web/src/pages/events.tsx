import { useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { isAdmin, useBus } from '../store';
import { Icon } from '../ui/icons';
import { Badge, Btn, Card, confirmDialog, Empty, Input, Modal, Select, toast } from '../ui/kit';

const TYPE_LABEL: Record<string, string> = {
  intrusion: 'Intrusion', loitering: 'Loitering', line_cross: 'Line crossing', count: 'Crowd / count', presence: 'Object detected',
  track: 'Object record', motion: 'Motion', test: 'Test',
};

export const eventTitle = (e: any) => {
  const l = e.label ? e.label.replace(/_/g, ' ') : '';
  return l ? `${l[0].toUpperCase()}${l.slice(1)}` : TYPE_LABEL[e.type] || e.type;
};

export const imgUrl = (e: any, name: string) => `/api/events/${e.id}/${e.images[name]}`;

export function EventThumb(p: { e: any; small?: boolean }) {
  const e = p.e;
  const main = e.images?.snapshot ? imgUrl(e, 'snapshot') : e.images?.object ? imgUrl(e, 'object') : '';
  const crop = e.images?.plate ? imgUrl(e, 'plate') : e.images?.snapshot && e.images?.object ? imgUrl(e, 'object') : '';
  if (p.small) return main ? <img src={main} loading="lazy" /> : <div class="ph" />;
  return (
    <div class="thumb" style={main ? { backgroundImage: `url(${main})` } : {}}>
      {crop && <img class="crop" src={crop} loading="lazy" />}
    </div>
  );
}

export function EventDetail(p: { e: any; onClose: () => void }) {
  const e = p.e;
  const imgs = Object.keys(e.images || {});
  const [sel, setSel] = useState(imgs.includes('snapshot') ? 'snapshot' : imgs[0]);
  return (
    <Modal wide title={<>{eventTitle(e)} <span class="dim small">· {e.time}</span></>} onClose={p.onClose}>
      <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) 280px' }}>
        <div class="col">
          {sel ? (
            <div class="player" style={{ aspectRatio: 'auto', minHeight: 200 }}>
              <img src={imgUrl(e, sel)} style={{ position: 'static', width: '100%', height: 'auto' }} />
              {sel === 'snapshot' && e.box?.length === 4 && (
                <svg class="ov" viewBox="0 0 1 1" preserveAspectRatio="none">
                  <rect x={e.box[0]} y={e.box[1]} width={e.box[2] - e.box[0]} height={e.box[3] - e.box[1]} fill="none" stroke="#38bdf8" stroke-width="0.004" />
                </svg>
              )}
            </div>
          ) : <Empty icon="camera">No picture stored for this event</Empty>}
          {imgs.length > 1 && (
            <div class="row">
              {imgs.map((n) => (
                <img src={imgUrl(e, n)} onClick={() => setSel(n)} title={n}
                  style={{ height: 64, borderRadius: 8, cursor: 'pointer', border: `2px solid ${n === sel ? 'var(--accent)' : 'var(--line)'}`, background: '#000' }} />
              ))}
            </div>
          )}
        </div>
        <div class="col">
          {(e.attributes?.plate_text || e.attributes?.plate) && (
            <div style={{ textAlign: 'center', padding: 8 }}><span class="plate" style={{ fontSize: 22, padding: '4px 14px' }}>{e.attributes.plate_text || e.attributes.plate}</span></div>
          )}
          <dl class="kv">
            <dt>Type</dt><dd>{TYPE_LABEL[e.type] || e.type}</dd>
            {e.rule && <><dt>Rule</dt><dd>{e.rule}</dd></>}
            {e.label && <><dt>Object</dt><dd>{e.label} {e.track_id ? `#${e.track_id}` : ''}</dd></>}
            {e.score > 0 && <><dt>Confidence</dt><dd>{Math.round(e.score * 100)}%</dd></>}
            {Object.entries(e.attributes || {}).map(([k, v]) => <><dt>{k.replace(/_/g, ' ')}</dt><dd>{String(v)}</dd></>)}
            {Object.entries(e.extra || {}).map(([k, v]) => <><dt>{k.replace(/_/g, ' ')}</dt><dd>{String(v)}</dd></>)}
            <dt>Event ID</dt><dd class="mono">{e.id}</dd>
          </dl>
          {sel && <a class="btn sm" href={imgUrl(e, sel)} download={`${e.id}_${sel}.jpg`}><Icon n="download" s={15} /> Download picture</a>}
          {e.day && e.time && <a class="btn sm" href={`#/playback?date=${e.day}&t=${e.time.slice(11, 19).replace(/:/g, '')}`} onClick={p.onClose}><Icon n="playback" s={15} /> Play the recording</a>}
        </div>
      </div>
    </Modal>
  );
}

export function EventsPage() {
  const [list, setList] = useState<any[]>([]);
  const [loading, setLoading] = useState(true);
  const [q, setQ] = useState('');
  const [type, setType] = useState('');
  const [label, setLabel] = useState('');
  const [date, setDate] = useState('');
  const [open, setOpen] = useState<any>(null);
  const [more, setMore] = useState(true);
  const params = (before = '') => {
    const u = new URLSearchParams({ limit: '60' });
    if (q) u.set('q', q);
    if (type) u.set('type', type);
    if (label) u.set('label', label);
    if (date) u.set('date', date);
    if (before) u.set('before', before);
    return u.toString();
  };
  const load = () => {
    setLoading(true);
    api.get(`/api/events?${params()}`).then((l) => {
      setList(l);
      setMore(l.length === 60);
    }, (e) => toast(e.message, 'err')).finally(() => setLoading(false));
  };
  useEffect(() => {
    const t = setTimeout(load, 250);
    return () => clearTimeout(t);
  }, [q, type, label, date]);
  useBus('event', (e) => !q && !type && !label && !date && setList((l) => [e, ...l]), [q, type, label, date]);
  const loadMore = () => api.get(`/api/events?${params(list[list.length - 1]?.id)}`).then((l) => {
    setList((x) => [...x, ...l]);
    setMore(l.length === 60);
  });
  const labels = [...new Set(list.map((e) => e.label).filter(Boolean))];

  return (
    <div class="col">
      <div class="card" style={{ padding: 12 }}>
        <div class="row">
          <div style={{ flex: '1 1 240px' }}><Input value={q} onInput={setQ} placeholder="Search plate, object, rule…" /></div>
          <div style={{ width: 170 }}><Select value={type} onChange={setType} options={[['', 'All types'], ...Object.entries(TYPE_LABEL)] as any} /></div>
          <div style={{ width: 150 }}><Select value={label} onChange={setLabel} options={[['', 'All objects'], ...labels.map((l) => [l, l])] as any} /></div>
          <div style={{ width: 160 }}><Input type="date" value={date} onInput={setDate} /></div>
          <a class="btn" href={`/api/events/export.csv?${params()}`}><Icon n="download" s={16} /> CSV</a>
          {isAdmin() && (
            <Btn kind="danger" icon="trash" onClick={async () => {
              if (!(await confirmDialog('Delete all events', 'All stored events and their pictures will be removed. This cannot be undone.', 'Delete all', true))) return;
              await api.del('/api/events');
              setList([]);
              toast('Events deleted', 'ok');
            }} />
          )}
        </div>
      </div>
      {loading && !list.length ? <div class="empty"><span class="spin" /></div> : !list.length ? (
        <Card><Empty icon="events">No events match. Events appear here when AI rules or motion detection trigger.</Empty></Card>
      ) : (
        <div class="ev-grid">
          {list.map((e) => (
            <div class="ev" onClick={() => setOpen(e)}>
              <EventThumb e={e} />
              <div class="meta">
                <div class="t">
                  {eventTitle(e)}
                  {(e.attributes?.plate_text || e.attributes?.plate) && <span class="plate">{e.attributes.plate_text || e.attributes.plate}</span>}
                </div>
                <div class="row small dim" style={{ gap: 6 }}>
                  <span>{e.time?.replace('T', ' ').slice(5, 19)}</span>
                  <span class="grow" />
                  <Badge kind={e.type === 'motion' ? '' : 'accent'}>{TYPE_LABEL[e.type] || e.type}</Badge>
                </div>
                {e.rule && <div class="small muted">{e.rule}</div>}
              </div>
            </div>
          ))}
        </div>
      )}
      {more && list.length > 0 && <div class="row" style={{ justifyContent: 'center' }}><Btn onClick={loadMore}>Load more</Btn></div>}
      {open && <EventDetail e={open} onClose={() => setOpen(null)} />}
    </div>
  );
}
