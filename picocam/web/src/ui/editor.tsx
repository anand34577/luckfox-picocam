import { useRef, useState } from 'preact/hooks';
import { Btn } from './kit';

type Pt = [number, number];

// Draws on a camera still. Coordinates are normalised 0..1.
//   polygon: click to add points, drag to move, right-click a point to delete
//   line:    two points (A -> B), drag to move
//   rects:   drag to draw rectangles, click one to select, Delete to remove
export function ShapeEditor(p: {
  img: string;
  mode: 'polygon' | 'line' | 'rects';
  value: any;
  onChange: (v: any) => void;
  onRefresh?: () => void;
  extra?: any[];  // other shapes to show faintly: {zone?, line?}
}) {
  const svg = useRef<SVGSVGElement>(null);
  const [drag, setDrag] = useState<number | null>(null);
  const [draft, setDraft] = useState<{ x: number; y: number; w: number; h: number } | null>(null);
  const [sel, setSel] = useState<number | null>(null);
  const W = 1000, H = 562;

  const pos = (e: MouseEvent): Pt => {
    const r = svg.current!.getBoundingClientRect();
    return [Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)), Math.max(0, Math.min(1, (e.clientY - r.top) / r.height))];
  };
  const round = (v: number) => Math.round(v * 1000) / 1000;

  const pts: Pt[] = p.mode === 'rects' ? [] : (p.value || []);
  const rects: any[] = p.mode === 'rects' ? p.value || [] : [];

  const down = (e: MouseEvent) => {
    const [x, y] = pos(e);
    if (p.mode === 'rects') {
      setDraft({ x, y, w: 0, h: 0 });
      return;
    }
    if (p.mode === 'line') {
      if (pts.length < 2) p.onChange([...pts, [round(x), round(y)]]);
      return;
    }
    p.onChange([...pts, [round(x), round(y)]]);
  };
  const move = (e: MouseEvent) => {
    const [x, y] = pos(e);
    if (drag !== null) {
      const n = pts.map((q, i) => (i === drag ? [round(x), round(y)] : q));
      p.onChange(n);
    } else if (draft) {
      setDraft({ ...draft, w: x - draft.x, h: y - draft.y });
    }
  };
  const up = () => {
    setDrag(null);
    if (draft) {
      const r = { x: Math.min(draft.x, draft.x + draft.w), y: Math.min(draft.y, draft.y + draft.h), w: Math.abs(draft.w), h: Math.abs(draft.h) };
      if (r.w > 0.01 && r.h > 0.01) p.onChange([...rects, { x: round(r.x), y: round(r.y), w: round(r.w), h: round(r.h), enabled: true }]);
      setDraft(null);
    }
  };

  return (
    <div class="col" style={{ gap: 8 }}>
      <div class="editor">
        {p.img ? <img src={p.img} /> : <div class="msg" style={{ position: 'absolute', inset: 0, display: 'grid', placeItems: 'center', color: '#94a3b8' }}><span class="spin" /></div>}
        <svg ref={svg} viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" onMouseDown={down} onMouseMove={move} onMouseUp={up} onMouseLeave={up}
          onKeyDown={(e) => e.key === 'Delete' && sel !== null && (p.onChange(rects.filter((_, i) => i !== sel)), setSel(null))} tabIndex={0}>
          {(p.extra || []).map((x) => (
            <g opacity="0.45">
              {x.zone?.length >= 3 && <polygon points={x.zone.map((q: Pt) => `${q[0] * W},${q[1] * H}`).join(' ')} fill="none" stroke="#94a3b8" stroke-width="2" stroke-dasharray="6 4" />}
              {x.line?.length === 2 && <line x1={x.line[0][0] * W} y1={x.line[0][1] * H} x2={x.line[1][0] * W} y2={x.line[1][1] * H} stroke="#94a3b8" stroke-width="2" stroke-dasharray="6 4" />}
            </g>
          ))}
          {p.mode === 'polygon' && pts.length > 0 && (
            <polygon points={pts.map((q) => `${q[0] * W},${q[1] * H}`).join(' ')} fill="rgba(56,189,248,.18)" stroke="#38bdf8" stroke-width="3" />
          )}
          {p.mode === 'line' && pts.length === 2 && (
            <>
              <line x1={pts[0][0] * W} y1={pts[0][1] * H} x2={pts[1][0] * W} y2={pts[1][1] * H} stroke="#f43f5e" stroke-width="4" />
              {/* direction arrow: left side of A->B is "A side" */}
              <text x={pts[0][0] * W + 10} y={pts[0][1] * H - 10} fill="#fff" font-size="22" font-weight="700" style={{ paintOrder: 'stroke', stroke: '#f43f5e', strokeWidth: 4 }}>A</text>
              <text x={pts[1][0] * W + 10} y={pts[1][1] * H - 10} fill="#fff" font-size="22" font-weight="700" style={{ paintOrder: 'stroke', stroke: '#f43f5e', strokeWidth: 4 }}>B</text>
            </>
          )}
          {pts.map((q, i) => (
            <circle cx={q[0] * W} cy={q[1] * H} r="9" fill="#fff" stroke={p.mode === 'line' ? '#f43f5e' : '#38bdf8'} stroke-width="3" style={{ cursor: 'move' }}
              onMouseDown={(e) => {
                e.stopPropagation();
                setDrag(i);
              }}
              onContextMenu={(e) => {
                e.preventDefault();
                p.onChange(pts.filter((_, j) => j !== i));
              }} />
          ))}
          {rects.map((r, i) => (
            <rect x={r.x * W} y={r.y * H} width={r.w * W} height={r.h * H} fill="rgba(15,23,42,.75)" stroke={i === sel ? '#38bdf8' : '#64748b'} stroke-width="3"
              onMouseDown={(e) => {
                e.stopPropagation();
                setSel(i);
              }} />
          ))}
          {draft && <rect x={Math.min(draft.x, draft.x + draft.w) * W} y={Math.min(draft.y, draft.y + draft.h) * H} width={Math.abs(draft.w) * W} height={Math.abs(draft.h) * H} fill="rgba(56,189,248,.2)" stroke="#38bdf8" stroke-width="2" />}
        </svg>
      </div>
      <div class="row small muted">
        <span class="grow">
          {p.mode === 'polygon' && 'Click to add corners, drag to move, right-click a corner to remove it.'}
          {p.mode === 'line' && 'Click two points, A then B. Walk across once and watch the Live counters to see which direction is "A→B".'}
          {p.mode === 'rects' && 'Drag to draw a box. Click a box and press Delete to remove it.'}
        </span>
        {p.onRefresh && <Btn sm kind="ghost" icon="refresh" onClick={p.onRefresh}>New picture</Btn>}
        {p.mode === 'rects' && sel !== null && <Btn sm kind="ghost" icon="trash" onClick={() => (p.onChange(rects.filter((_, i) => i !== sel)), setSel(null))}>Remove box</Btn>}
        <Btn sm kind="ghost" icon="x" onClick={() => p.onChange([])}>Clear</Btn>
      </div>
    </div>
  );
}
