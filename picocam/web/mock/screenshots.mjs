// Renders the screenshots and the social preview used in the docs from the mock camera. No real footage involved.
//   npm run build && npm run mock &   then   npm run screenshots
// Needs Chrome/Chromium: set CHROME=/path/to/chrome if it is not found.
import puppeteer from 'puppeteer-core';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = fileURLToPath(new URL('.', import.meta.url));
const out = join(here, '..', '..', '..', 'docs', 'images');
mkdirSync(out, { recursive: true });
const base = process.env.MOCK || 'http://localhost:8099';
const chrome = [process.env.CHROME, 'C:/Program Files/Google/Chrome/Application/chrome.exe', '/usr/bin/google-chrome', '/usr/bin/chromium', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'].find((p) => p && existsSync(p));
if (!chrome) throw new Error('Chrome not found: set CHROME=/path/to/chrome');

const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ['--no-sandbox'] });

async function page(width, height, theme, scale = 1) {
  const pg = await browser.newPage();
  await pg.setViewport({ width, height, deviceScaleFactor: scale });
  await pg.evaluateOnNewDocument((t) => { try { localStorage.setItem('pc-theme', t); localStorage.setItem('pc-stream', 'sub'); } catch {} }, theme);
  return pg;
}
// the live picture is a stream the mock cannot send: show the generated scene in its place
const liveStyle = '.player{background:#000 url(/snapshot.jpg) center/contain no-repeat !important}.player .msg{display:none !important}.player canvas,.player video{display:none}';

async function shot(name, hash, { width = 1440, height = 900, theme = 'dark', live = false, wait = 1200, full = false, scale = 1 } = {}) {
  const pg = await page(width, height, theme, scale);
  const errors = [];
  pg.on('pageerror', (e) => errors.push(e.message));
  await pg.goto(`${base}/#/${hash}`, { waitUntil: 'networkidle0' });
  if (live) await pg.addStyleTag({ content: liveStyle });
  await new Promise((r) => setTimeout(r, wait));
  await pg.screenshot({ path: join(out, `${name}.png`), fullPage: full });
  await pg.close();
  console.log('wrote', `${name}.png`, errors.length ? 'ERRORS: ' + errors.join('; ') : '');
}

await shot('ui-live', 'live', { live: true });
await shot('ui-events', 'events');
await shot('ui-ai', 'ai');
await shot('ui-video', 'video');
await shot('ui-image', 'image', { live: true });
await shot('ui-storage', 'storage');
await shot('ui-network', 'network', { height: 1150 });
await shot('ui-system', 'system');
await shot('ui-dashboard', 'dashboard');
await shot('ui-live-light', 'live', { live: true, theme: 'light' });
await shot('ui-live-phone', 'live', { width: 390, height: 844, live: true, scale: 2 });

// social preview (1280x640, what GitHub shows when the repo is shared) and the README banner
const shotUri = 'data:image/png;base64,' + readFileSync(join(out, 'ui-live.png')).toString('base64');
const html = readFileSync(join(here, 'branding.html'), 'utf8').replace('%%SHOT%%', shotUri);
for (const [name, w, h] of [['social-preview', 1280, 640], ['banner', 1600, 480]]) {
  const pg = await page(w, h, 'dark');
  await pg.setContent(html.replaceAll('%%LAYOUT%%', name), { waitUntil: 'load' });
  await pg.evaluate(() => new Promise((r) => setTimeout(r, 300)));
  await pg.screenshot({ path: join(out, `${name}.png`) });
  await pg.close();
  console.log('wrote', `${name}.png`);
}
await browser.close();
