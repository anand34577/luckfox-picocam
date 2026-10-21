import { ComponentChildren, render } from 'preact';
import { useCallback, useEffect, useState } from 'preact/hooks';
import { api } from '../api';
import { Icon } from './icons';

// ---------------------------------------------------------------- layout

export function Card(p: { title?: ComponentChildren; hint?: ComponentChildren; actions?: ComponentChildren; children?: ComponentChildren; tight?: boolean; class?: string }) {
  return (
    <div class={'card ' + (p.class || '')}>
      {(p.title || p.actions) && (
        <div class="card-h">
          <h3>{p.title}</h3>
          {p.hint && <span class="hint">{p.hint}</span>}
          <span class="grow" />
          {p.actions}
        </div>
      )}
      <div class={'card-b' + (p.tight ? ' tight' : '')}>{p.children}</div>
    </div>
  );
}

export function Field(p: { label: ComponentChildren; help?: ComponentChildren; children: ComponentChildren; class?: string }) {
  return (
    <div class={'field ' + (p.class || '')}>
      <label>{p.label}</label>
      {p.children}
      {p.help && <div class="help">{p.help}</div>}
    </div>
  );
}

export function Stat(p: { k: ComponentChildren; v: ComponentChildren; unit?: string; pct?: number; icon?: string }) {
  const lvl = p.pct === undefined ? '' : p.pct > 90 ? 'bad' : p.pct > 75 ? 'warn' : '';
  return (
    <div class="card stat">
      <div class="k">
        {p.icon && <Icon n={p.icon} s={14} />}
        {p.k}
      </div>
      <div class="v">
        {p.v}
        {p.unit && <small>{p.unit}</small>}
      </div>
      {p.pct !== undefined && (
        <div class={'bar ' + lvl}>
          <i style={{ width: `${Math.max(0, Math.min(100, p.pct))}%` }} />
        </div>
      )}
    </div>
  );
}

export function Empty(p: { icon?: string; children: ComponentChildren }) {
  return (
    <div class="empty">
      <Icon n={p.icon || 'info'} s={34} w={1.4} />
      <div>{p.children}</div>
    </div>
  );
}

export function Callout(p: { kind?: 'warn' | 'bad' | 'ok'; children: ComponentChildren; icon?: string }) {
  return (
    <div class={'callout ' + (p.kind || '')}>
      <Icon n={p.icon || (p.kind === 'ok' ? 'check' : p.kind ? 'warn' : 'info')} s={17} />
      <div>{p.children}</div>
    </div>
  );
}

// ---------------------------------------------------------------- inputs

export function Input(p: { value: any; onInput: (v: string) => void; type?: string; placeholder?: string; disabled?: boolean; unit?: string; min?: number; max?: number; step?: number; mono?: boolean }) {
  const el = (
    <input class={'input' + (p.mono ? ' mono' : '')} type={p.type || 'text'} value={p.value ?? ''} placeholder={p.placeholder} disabled={p.disabled}
      min={p.min} max={p.max} step={p.step} onInput={(e) => p.onInput((e.target as HTMLInputElement).value)} />
  );
  return p.unit ? <div class="unit">{el}<span>{p.unit}</span></div> : el;
}

export function Num(p: { value: number; onChange: (v: number) => void; min?: number; max?: number; step?: number; unit?: string; disabled?: boolean }) {
  return <Input type="number" value={p.value} min={p.min} max={p.max} step={p.step} unit={p.unit} disabled={p.disabled}
    onInput={(v) => v !== '' && !isNaN(+v) && p.onChange(+v)} />;
}

export function Select(p: { value: any; onChange: (v: string) => void; options: (string | [string, string])[]; disabled?: boolean }) {
  return (
    <select class="input" value={p.value} disabled={p.disabled} onChange={(e) => p.onChange((e.target as HTMLSelectElement).value)}>
      {p.options.map((o) => (Array.isArray(o) ? <option value={o[0]}>{o[1]}</option> : <option value={o}>{o}</option>))}
    </select>
  );
}

export function Slider(p: { value: number; onChange: (v: number) => void; min?: number; max?: number; step?: number; unit?: string; disabled?: boolean }) {
  const min = p.min ?? 0, max = p.max ?? 100;
  const pct = ((p.value - min) / (max - min)) * 100;
  return (
    <div class="slider">
      <input type="range" min={min} max={max} step={p.step ?? 1} value={p.value} disabled={p.disabled} style={{ '--p': `${pct}%` } as any}
        onInput={(e) => p.onChange(+(e.target as HTMLInputElement).value)} />
      <span class="val">{p.value}{p.unit || ''}</span>
    </div>
  );
}

export function Switch(p: { checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  return (
    <label class="switch">
      <input type="checkbox" checked={!!p.checked} disabled={p.disabled} onChange={(e) => p.onChange((e.target as HTMLInputElement).checked)} />
      <i />
    </label>
  );
}

export function ToggleRow(p: { title: ComponentChildren; desc?: ComponentChildren; checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  return (
    <div class="toggle-row">
      <div>
        <div class="t">{p.title}</div>
        {p.desc && <div class="d">{p.desc}</div>}
      </div>
      <Switch checked={p.checked} onChange={p.onChange} disabled={p.disabled} />
    </div>
  );
}

export function Seg(p: { value: string; onChange: (v: string) => void; options: (string | [string, string])[]; disabled?: boolean }) {
  return (
    <div class="seg">
      {p.options.map((o) => {
        const [v, l] = Array.isArray(o) ? o : [o, o];
        return (
          <button type="button" class={p.value === v ? 'on' : ''} disabled={p.disabled} onClick={() => p.onChange(v)}>{l}</button>
        );
      })}
    </div>
  );
}

export function Chips(p: { all: string[]; value: string[]; onChange: (v: string[]) => void; empty?: string }) {
  const toggle = (c: string) => p.onChange(p.value.includes(c) ? p.value.filter((x) => x !== c) : [...p.value, c]);
  return (
    <div class="chips">
      {p.all.map((c) => (
        <span class={'chip' + (p.value.includes(c) ? ' on' : '')} onClick={() => toggle(c)}>{c.replace(/_/g, ' ')}</span>
      ))}
      {!p.all.length && <span class="dim small">{p.empty || 'none'}</span>}
    </div>
  );
}

export function Tabs(p: { value: string; onChange: (v: string) => void; tabs: [string, string][] }) {
  return (
    <div class="tabs">
      {p.tabs.map(([v, l]) => (
        <button class={p.value === v ? 'on' : ''} onClick={() => p.onChange(v)}>{l}</button>
      ))}
    </div>
  );
}

export function Btn(p: { children?: ComponentChildren; onClick?: () => any; kind?: string; icon?: string; disabled?: boolean; title?: string; type?: string; sm?: boolean }) {
  const [busy, setBusy] = useState(false);
  const click = async () => {
    if (!p.onClick) return;
    const r = p.onClick();
    if (r && typeof r.then === 'function') {
      setBusy(true);
      try {
        await r;
      } finally {
        setBusy(false);
      }
    }
  };
  return (
    <button type={(p.type as any) || 'button'} class={`btn ${p.kind || ''} ${p.sm ? 'sm' : ''} ${!p.children ? 'icon' : ''}`} disabled={p.disabled || busy} title={p.title} onClick={click}>
      {busy ? <span class="spin" /> : p.icon && <Icon n={p.icon} s={p.sm ? 15 : 17} />}
      {p.children}
    </button>
  );
}

export function Badge(p: { kind?: string; children: ComponentChildren }) {
  return <span class={'badge ' + (p.kind || '')}>{p.children}</span>;
}

export function Copy(p: { text: string }) {
  return (
    <div class="copy">
      <span title={p.text}>{p.text}</span>
      <Btn sm kind="ghost" icon="copy" title="Copy" onClick={() => {
        navigator.clipboard?.writeText(p.text).then(() => toast('Copied'), () => toast(p.text));
      }} />
    </div>
  );
}

// ---------------------------------------------------------------- modal / confirm / toast

export function Modal(p: { title: ComponentChildren; onClose: () => void; children: ComponentChildren; footer?: ComponentChildren; wide?: boolean }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => e.key === 'Escape' && p.onClose();
    addEventListener('keydown', k);
    return () => removeEventListener('keydown', k);
  }, []);
  return (
    <div class="modal-bg" onMouseDown={(e) => e.target === e.currentTarget && p.onClose()}>
      <div class={'modal' + (p.wide ? ' wide' : '')}>
        <div class="modal-h">
          <h3>{p.title}</h3>
          <Btn sm kind="ghost" icon="x" onClick={p.onClose} />
        </div>
        <div class="modal-b">{p.children}</div>
        {p.footer && <div class="modal-f">{p.footer}</div>}
      </div>
    </div>
  );
}

export function confirmDialog(title: string, text: ComponentChildren, okText = 'Confirm', danger = false): Promise<boolean> {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const done = (v: boolean) => {
      render(null, host);
      host.remove();
      resolve(v);
    };
    render(
      <Modal title={title} onClose={() => done(false)} footer={
        <>
          <Btn kind="ghost" onClick={() => done(false)}>Cancel</Btn>
          <Btn kind={danger ? 'danger solid' : 'primary'} onClick={() => done(true)}>{okText}</Btn>
        </>
      }>
        <div class="muted">{text}</div>
      </Modal>,
      host,
    );
  });
}

let toastHost: HTMLDivElement | null = null;
export function toast(msg: string, kind: '' | 'ok' | 'err' = '') {
  if (!toastHost) {
    toastHost = document.createElement('div');
    toastHost.className = 'toasts';
    document.body.appendChild(toastHost);
  }
  const t = document.createElement('div');
  t.className = 'toast ' + kind;
  t.textContent = msg;
  toastHost.appendChild(t);
  setTimeout(() => t.remove(), kind === 'err' ? 6000 : 3000);
}

// ---------------------------------------------------------------- settings section hook

// Loads a settings section; edits stay local until save(). Returns [value, set, save, dirty, reload].
export function useSection<T = any>(section: string) {
  const [orig, setOrig] = useState<T | null>(null);
  const [val, setVal] = useState<T | null>(null);
  const load = useCallback(() => {
    api.get<T>(`/api/config/${section}`).then((v) => {
      setOrig(v);
      setVal(JSON.parse(JSON.stringify(v)));
    }, (e) => toast(e.message, 'err'));
  }, [section]);
  useEffect(load, [load]);
  const set = (patch: Partial<T> | ((v: T) => T)) =>
    setVal((cur) => (typeof patch === 'function' ? (patch as any)(cur) : { ...(cur as any), ...patch }));
  const save = async (replace = false) => {
    try {
      const r = replace ? await api.post<T>(`/api/config/${section}`, val) : await api.put<T>(`/api/config/${section}`, val);
      setOrig(r);
      setVal(JSON.parse(JSON.stringify(r)));
      toast('Saved', 'ok');
      return true;
    } catch (e: any) {
      toast(e.message, 'err');
      return false;
    }
  };
  const dirty = JSON.stringify(orig) !== JSON.stringify(val);
  return { val, set, save, dirty, reload: load, setVal };
}

// immutable nested update: upd(obj, 'a.b.c', v)
export function upd<T>(obj: T, path: string, v: any): T {
  const keys = path.split('.');
  const root: any = Array.isArray(obj) ? [...(obj as any)] : { ...(obj as any) };
  let cur = root;
  for (let i = 0; i < keys.length - 1; i++) {
    const k = keys[i];
    cur[k] = Array.isArray(cur[k]) ? [...cur[k]] : { ...(cur[k] || {}) };
    cur = cur[k];
  }
  cur[keys[keys.length - 1]] = v;
  return root;
}

export function SaveBar(p: { dirty: boolean; onSave: () => any; onReset: () => void; note?: string }) {
  if (!p.dirty) return null;
  return (
    <div class="card" style={{ position: 'sticky', bottom: 16, marginTop: 18, padding: '10px 16px', display: 'flex', gap: 12, alignItems: 'center', boxShadow: 'var(--shadow)', zIndex: 10 }}>
      <Icon n="info" s={16} />
      <span class="muted">{p.note || 'You have unsaved changes'}</span>
      <span class="grow" />
      <Btn kind="ghost" onClick={p.onReset}>Discard</Btn>
      <Btn kind="primary" icon="check" onClick={p.onSave}>Save changes</Btn>
    </div>
  );
}
