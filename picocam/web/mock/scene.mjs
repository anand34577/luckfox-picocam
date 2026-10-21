// The picture for the demo/mock camera: a stock street photo (street.jpg, "Tram riding along wet city street near modern
// cars" by Vlad Fonsark, Pexels licence, see NOTICE.md) with the real number plates covered by fictional ones
// ("MH 00 PC 0000": RTO code 00 does not exist). Not footage from a PicoCam.
import { readFileSync } from 'node:fs';

export const DEMO_PLATE = 'MH00PC0000';
// an SVG used as an image cannot load external files: the photo goes in as a data URI
const photo = 'data:image/jpeg;base64,' + readFileSync(new URL('./street.jpg', import.meta.url)).toString('base64');

// a fictional plate drawn over a real one (pixel box in the 1920x1080 photo)
const plate = (x, y, w, h, text) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="2" fill="#f2f2ec" stroke="#222" stroke-width="1.5"/>
  <text x="${x + w / 2}" y="${y + h * 0.78}" text-anchor="middle" font-family="monospace" font-weight="700" font-size="${h * 0.8}" textLength="${w * 0.9}" fill="#111">${text}</text>`;

export function sceneSvg(w = 1920, h = 1080, opts = {}) {
  const osd = opts.osd !== false;
  const stamp = opts.stamp || '2026-10-21  18:40:12';
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1920 1080" width="${w}" height="${h}">
  <image href="${photo}" width="1920" height="1080"/>
  ${plate(676, 709, 101, 27, opts.plate || 'MH 00 PC 0000')}
  ${plate(1658, 612, 81, 26, 'MH 00 PC 0007')}
  ${osd ? `<g font-family="monospace" font-size="38" fill="#fff" stroke="#000" stroke-width="5" paint-order="stroke"><text x="30" y="56">${stamp}</text><text x="30" y="1050">Front gate</text></g>` : ''}
</svg>`;
}

// a crop of the scene as an event picture
export function cropSvg(kind) {
  if (kind === 'plate') {
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 130"><rect width="400" height="130" fill="#1b2029"/><rect x="14" y="14" width="372" height="102" rx="10" fill="#f5f5f0" stroke="#111" stroke-width="5"/><rect x="26" y="26" width="30" height="78" fill="#0b4fa3"/><text x="215" y="86" text-anchor="middle" font-family="monospace" font-weight="800" font-size="58" fill="#111" letter-spacing="3">MH 00 PC 0000</text></svg>`;
  }
  if (kind === 'object') {
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="550 540 360 270">${sceneSvg(1920, 1080, { osd: false }).replace(/^<svg[^>]*>/, '').replace(/<\/svg>$/, '')}</svg>`;
  }
  return sceneSvg(1920, 1080);
}

// normalised boxes for the detections drawn on the scene (x1,y1,x2,y2)
export const SCENE_BOXES = {
  car: [0.299, 0.518, 0.461, 0.741],
  person: [0.242, 0.474, 0.313, 0.755],
  car2: [0.786, 0.491, 0.944, 0.69],
  plate: [0.352, 0.656, 0.405, 0.682],
};
