import { useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { canEdit, status, useBus, useStore } from '../store';
import { Icon } from '../ui/icons';
import { Badge, Btn, Card, Empty, Seg, Switch } from '../ui/kit';
import { EventDetail, EventThumb, eventTitle } from './events';
import { LiveViewer } from '../ui/viewer';

export function LivePage() {
  const [stream, setStream] = useState<'main' | 'sub'>((localStorage.getItem('pc-stream') as any) || 'sub');
  const [ai, setAi] = useState(localStorage.getItem('pc-ai') !== '0');
  const [zones, setZones] = useState(localStorage.getItem('pc-zones') !== '0');
  const [rules, setRules] = useState<any[]>([]);
  const [feed, setFeed] = useState<any[]>([]);
  const [open, setOpen] = useState<any>(null);
  const [motion, setMotion] = useState<any>(null);
  const st = useStore(status);

  useEffect(() => {
    if (canEdit()) api.get('/api/config/rules').then(setRules, () => {});
    api.get('/api/events?limit=12').then(setFeed, () => {});
  }, []);
  useBus('event', (e) => setFeed((f) => [e, ...f].slice(0, 30)));
  useBus('motion', setMotion);
  const pick = (k: string, v: string, set: (x: any) => void) => {
    set(v);
    localStorage.setItem(k, v);
  };

  const aiSt = st?.ai;
  const isp = st?.isp;
  // the camera burns the boxes into the sub stream: drawing ours on top would show every box twice
  const burned = stream === 'sub' && !!st?.video?.sub?.ai_overlay;
  return (
    <div class="grid split" style={{ gridTemplateColumns: 'minmax(0,1fr) 340px', alignItems: 'start' }}>
      <div class="col">
        <LiveViewer stream={stream} showAi={ai && !burned} rules={zones ? rules : []} />
        <div class="row">
          <Seg value={stream} onChange={(v) => pick('pc-stream', v, setStream)} options={[['main', 'Main stream'], ['sub', 'Sub stream']]} />
          <span class="grow" />
          {burned ? (
            <span class="small muted" title="Video & Audio > Sub stream > Burn AI boxes into the sub stream">AI boxes: in the video</span>
          ) : (
            <label class="row small muted" style={{ gap: 8 }}>
              <Switch checked={ai} onChange={(v) => pick('pc-ai', v ? '1' : '0', () => setAi(v))} /> AI boxes
            </label>
          )}
          <label class="row small muted" style={{ gap: 8 }}>
            <Switch checked={zones} onChange={(v) => pick('pc-zones', v ? '1' : '0', () => setZones(v))} /> Zones & lines
          </label>
        </div>
        <div class="grid g4">
          <div class="card stat">
            <div class="k"><Icon n="ai" s={14} /> AI</div>
            <div class="v">{aiSt?.running ? <>{aiSt.fps}<small>fps · {aiSt.infer_ms} ms</small></> : <small style={{ margin: 0 }}>{aiSt?.enabled ? 'error' : 'off'}</small>}</div>
          </div>
          <div class="card stat">
            <div class="k"><Icon n="eye" s={14} /> Objects tracked</div>
            <div class="v">{aiSt?.tracks ?? 0}</div>
          </div>
          <div class="card stat">
            <div class="k"><Icon n="sun" s={14} /> Exposure</div>
            <div class="v">{isp ? <>{isp.exposure_ms}<small>ms · ×{isp.gain}</small></> : '–'}</div>
          </div>
          <div class="card stat">
            <div class="k"><Icon n="motion" s={14} /> Motion</div>
            <div class="v">{motion ? <>{motion.level}<small>%</small></> : <small style={{ margin: 0 }}>{st?.motion?.enabled ? 'idle' : 'off'}</small>}</div>
          </div>
        </div>
        {aiSt?.counters?.length > 0 && (
          <Card title="Line counters" actions={<Btn sm kind="ghost" icon="refresh" onClick={() => api.post('/api/ai/counters/reset')}>Reset</Btn>}>
            <div class="grid g3">
              {aiSt.counters.map((c: any) => (
                <div class="stat card"><div class="k">{c.name}</div><div class="v">{c.a_to_b}<small>A→B</small> {c.b_to_a}<small>B→A</small></div></div>
              ))}
            </div>
          </Card>
        )}
      </div>
      <Card title="Live events" tight actions={<a href="#/events" class="small">View all</a>}>
        <div style={{ maxHeight: 640, overflowY: 'auto' }}>
          {!feed.length && <Empty icon="events">No events yet. Enable AI or motion detection to start capturing.</Empty>}
          {feed.map((e) => (
            <div class="feed-item" onClick={() => setOpen(e)}>
              <EventThumb e={e} small />
              <div style={{ minWidth: 0 }}>
                <div class="row" style={{ gap: 6 }}>
                  <b class="small">{eventTitle(e)}</b>
                  {(e.attributes?.plate_text || e.attributes?.plate) && <span class="plate">{e.attributes.plate_text || e.attributes.plate}</span>}
                </div>
                <div class="dim small">{e.time?.slice(11)} · {e.rule || e.type}</div>
              </div>
            </div>
          ))}
        </div>
        {aiSt?.error && <div style={{ padding: 12 }}><Badge kind="bad">{aiSt.error}</Badge></div>}
      </Card>
      {open && <EventDetail e={open} onClose={() => setOpen(null)} />}
    </div>
  );
}
