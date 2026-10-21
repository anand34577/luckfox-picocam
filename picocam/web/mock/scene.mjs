// Generated illustrations for the demo/mock camera. Everything here is drawn from code: no photographs, no real
// people, no real number plates. The plate text is fictional ("MH 00 PC 0000": RTO code 00 does not exist).
export const DEMO_PLATE = 'MH00PC0000';

function car(x, y, w, color, plate, lit) {
  const h = w * 0.46;
  return `<g transform="translate(${x} ${y})">
    <ellipse cx="${w / 2}" cy="${h * 1.02}" rx="${w * 0.52}" ry="${h * 0.09}" fill="#000" opacity=".35"/>
    <path d="M${w * 0.06} ${h * 0.78} L${w * 0.1} ${h * 0.45} Q${w * 0.14} ${h * 0.3} ${w * 0.3} ${h * 0.26} L${w * 0.7} ${h * 0.26} Q${w * 0.86} ${h * 0.3} ${w * 0.9} ${h * 0.45} L${w * 0.94} ${h * 0.78} Z" fill="${color}"/>
    <path d="M${w * 0.2} ${h * 0.46} L${w * 0.26} ${h * 0.12} Q${w * 0.3} ${h * 0.04} ${w * 0.4} ${h * 0.04} L${w * 0.6} ${h * 0.04} Q${w * 0.7} ${h * 0.04} ${w * 0.74} ${h * 0.12} L${w * 0.8} ${h * 0.46} Z" fill="${color}"/>
    <path d="M${w * 0.25} ${h * 0.44} L${w * 0.3} ${h * 0.14} L${w * 0.7} ${h * 0.14} L${w * 0.75} ${h * 0.44} Z" fill="#9bb4c9" opacity=".75"/>
    <rect x="${w * 0.08}" y="${h * 0.5}" width="${w * 0.14}" height="${h * 0.12}" rx="4" fill="${lit ? '#fff7c2' : '#d9dee5'}"/>
    <rect x="${w * 0.78}" y="${h * 0.5}" width="${w * 0.14}" height="${h * 0.12}" rx="4" fill="${lit ? '#fff7c2' : '#d9dee5'}"/>
    <rect x="${w * 0.06}" y="${h * 0.72}" width="${w * 0.88}" height="${h * 0.1}" rx="5" fill="#1a1f27"/>
    <rect x="${w * 0.38}" y="${h * 0.56}" width="${w * 0.24}" height="${h * 0.15}" rx="3" fill="#f5f5f0" stroke="#222" stroke-width="2"/>
    <text x="${w * 0.5}" y="${h * 0.675}" text-anchor="middle" font-family="monospace" font-weight="700" font-size="${w * 0.045}" fill="#111">${plate}</text>
    <circle cx="${w * 0.22}" cy="${h * 0.86}" r="${h * 0.14}" fill="#10141a"/><circle cx="${w * 0.22}" cy="${h * 0.86}" r="${h * 0.07}" fill="#8a94a3"/>
    <circle cx="${w * 0.78}" cy="${h * 0.86}" r="${h * 0.14}" fill="#10141a"/><circle cx="${w * 0.78}" cy="${h * 0.86}" r="${h * 0.07}" fill="#8a94a3"/>
  </g>`;
}

function person(x, y, s, color) {
  return `<g transform="translate(${x} ${y}) scale(${s})">
    <ellipse cx="0" cy="118" rx="26" ry="6" fill="#000" opacity=".3"/>
    <circle cx="0" cy="-8" r="13" fill="#d9b18f"/>
    <path d="M-17 8 Q0 0 17 8 L20 62 L-20 62 Z" fill="${color}"/>
    <rect x="-17" y="60" width="12" height="56" rx="5" fill="#2b3445"/><rect x="5" y="60" width="12" height="56" rx="5" fill="#2b3445"/>
  </g>`;
}

// the main picture: a gate and a road at dusk, a car approaching, a person on the pavement
export function sceneSvg(w = 1920, h = 1080, opts = {}) {
  const osd = opts.osd !== false;
  const stamp = opts.stamp || '2026-10-21  18:40:12';
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1920 1080" width="${w}" height="${h}">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#27324d"/><stop offset=".6" stop-color="#b8795a"/><stop offset="1" stop-color="#e3a56d"/></linearGradient>
    <linearGradient id="road" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4a4f58"/><stop offset="1" stop-color="#2a2d33"/></linearGradient>
  </defs>
  <rect width="1920" height="1080" fill="url(#sky)"/>
  <g fill="#1d2433" opacity=".9"><rect x="0" y="330" width="260" height="240"/><rect x="300" y="380" width="180" height="190"/><rect x="1500" y="300" width="240" height="270"/><rect x="1760" y="360" width="160" height="210"/></g>
  <g fill="#ffd27a" opacity=".8"><rect x="30" y="360" width="30" height="40"/><rect x="90" y="430" width="30" height="40"/><rect x="1540" y="340" width="30" height="40"/><rect x="1620" y="410" width="30" height="40"/><rect x="1800" y="400" width="30" height="40"/></g>
  <rect x="0" y="560" width="1920" height="40" fill="#555b66"/>
  <rect x="0" y="580" width="1920" height="500" fill="url(#road)"/>
  <g stroke="#9aa1ad" stroke-width="6" opacity=".7"><line x1="0" y1="630" x2="1920" y2="630"/></g>
  <g fill="#e5e7eb" opacity=".8"><polygon points="900,640 960,640 1020,1080 880,1080"/><polygon points="540,640 580,640 560,1080 380,1080"/><polygon points="1340,640 1380,640 1540,1080 1380,1080"/></g>
  <g><rect x="760" y="360" width="14" height="230" fill="#7b2d2d"/><rect x="1180" y="360" width="14" height="230" fill="#7b2d2d"/>
  <rect x="774" y="380" width="406" height="150" fill="none" stroke="#7b2d2d" stroke-width="10"/>
  <g stroke="#7b2d2d" stroke-width="8">${Array.from({ length: 13 }, (_, i) => `<line x1="${790 + i * 30}" y1="384" x2="${790 + i * 30}" y2="526"/>`).join('')}</g></g>
  <g fill="#2b3040"><rect x="100" y="240" width="12" height="330"/><rect x="1700" y="220" width="12" height="350"/></g>
  <circle cx="106" cy="236" r="20" fill="#fff3c4"/><circle cx="1706" cy="216" r="20" fill="#fff3c4"/>
  ${person(1640, 520, 2.4, '#c0492f')}
  ${car(560, 560, 600, '#2f6fb0', opts.plate || 'MH 00 PC 0000', true)}
  ${opts.noExtra ? '' : car(1280, 592, 330, '#b8b8c0', 'MH 00 PC 0007', false)}
  ${osd ? `<g font-family="monospace" font-size="38" fill="#fff" stroke="#000" stroke-width="5" paint-order="stroke"><text x="30" y="56">${stamp}</text><text x="30" y="1050">Front gate</text></g>` : ''}
</svg>`;
}

// a crop of the scene as an event picture
export function cropSvg(kind) {
  if (kind === 'plate') {
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 130"><rect width="400" height="130" fill="#1b2029"/><rect x="14" y="14" width="372" height="102" rx="10" fill="#f5f5f0" stroke="#111" stroke-width="5"/><rect x="26" y="26" width="30" height="78" fill="#0b4fa3"/><text x="215" y="86" text-anchor="middle" font-family="monospace" font-weight="800" font-size="58" fill="#111" letter-spacing="3">MH 00 PC 0000</text></svg>`;
  }
  if (kind === 'object') {
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="560 560 600 300">${sceneSvg(1920, 1080, { osd: false, noExtra: true }).replace(/^<svg[^>]*>/, '').replace(/<\/svg>$/, '')}</svg>`;
  }
  return sceneSvg(1920, 1080);
}

// normalised boxes for the detections drawn on the scene (x1,y1,x2,y2)
export const SCENE_BOXES = {
  car: [0.285, 0.515, 0.605, 0.785],
  person: [0.825, 0.43, 0.884, 0.75],
  car2: [0.665, 0.545, 0.842, 0.695],
  plate: [0.4, 0.63, 0.49, 0.68],
};
