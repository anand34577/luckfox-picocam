// REST client for the camera. The X-PicoCam-UI header stops the browser's own password prompt on 401.
export class ApiError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

type Listener = () => void;
const authListeners: Listener[] = [];
export const onAuthLost = (f: Listener) => authListeners.push(f);

async function req<T>(method: string, path: string, body?: unknown, raw?: BodyInit): Promise<T> {
  const headers: Record<string, string> = { 'X-PicoCam-UI': '1' };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const r = await fetch(path, {
    method,
    headers,
    body: raw ?? (body !== undefined ? JSON.stringify(body) : undefined),
    credentials: 'same-origin',
  });
  const text = await r.text();
  let data: any = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = text;
  }
  if (!r.ok) {
    if (r.status === 401 && !path.startsWith('/api/login')) authListeners.forEach((f) => f());
    throw new ApiError(r.status, (data && data.error) || r.statusText || 'request failed');
  }
  return data as T;
}

export const api = {
  get: <T = any>(p: string) => req<T>('GET', p),
  post: <T = any>(p: string, b?: unknown) => req<T>('POST', p, b ?? {}),
  put: <T = any>(p: string, b?: unknown) => req<T>('PUT', p, b ?? {}),
  del: <T = any>(p: string) => req<T>('DELETE', p),
  upload: <T = any>(p: string, file: Blob) => req<T>('POST', p, undefined, file),
};

// Upload with progress (models, firmware): XHR because fetch has no upload progress.
export function uploadWithProgress(path: string, file: Blob, onProgress: (pct: number) => void): Promise<any> {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', path);
    x.setRequestHeader('X-PicoCam-UI', '1');
    x.upload.onprogress = (e) => e.lengthComputable && onProgress(Math.round((e.loaded / e.total) * 100));
    x.onload = () => {
      let d: any = null;
      try {
        d = JSON.parse(x.responseText);
      } catch {}
      if (x.status >= 200 && x.status < 300) resolve(d);
      else reject(new ApiError(x.status, (d && d.error) || x.statusText));
    };
    x.onerror = () => reject(new ApiError(0, 'network error'));
    x.send(file);
  });
}

export function fmtBytes(n: number) {
  if (!n && n !== 0) return '–';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) (n /= 1024), i++;
  return `${n.toFixed(n >= 100 || i === 0 ? 0 : 1)} ${u[i]}`;
}

export function fmtDuration(s: number) {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m ${Math.floor(s % 60)}s`;
}

export function fmtTime(ms: number) {
  return new Date(ms).toLocaleString(undefined, { hour12: false });
}
