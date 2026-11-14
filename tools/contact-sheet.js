// Tiles the screenshots of one platform (or settings colour scheme) into a single PNG, to look over a run at once.
// usage: node tools/contact-sheet.js <dir with .png files> <out.png> [columns]
const fs = require('fs');
const path = require('path');
const png = require('./lib/png-diff');

const [dir, out, cols = '4'] = process.argv.slice(2);
if (!dir || !out) { console.error('usage: node tools/contact-sheet.js <dir> <out.png> [columns]'); process.exit(2); }
const images = fs.readdirSync(dir).filter(f => f.endsWith('.png')).sort().map(f => png.decode(fs.readFileSync(path.join(dir, f))));
if (!images.length) { console.error('no .png files in ' + dir); process.exit(1); }
const n = Number(cols), gap = 6;
const w = Math.max(...images.map(i => i.width)), h = Math.max(...images.map(i => i.height));
const rows = Math.ceil(images.length / n);
const sheet = { width: n * (w + gap) + gap, height: rows * (h + gap) + gap, data: null };
sheet.data = new Uint8Array(sheet.width * sheet.height * 4).fill(128);
for (let i = 3; i < sheet.data.length; i += 4) sheet.data[i] = 255;
images.forEach((img, k) => {
  const x0 = gap + (k % n) * (w + gap), y0 = gap + Math.floor(k / n) * (h + gap);
  for (let y = 0; y < img.height; y++) {
    sheet.data.set(img.data.subarray(y * img.width * 4, (y + 1) * img.width * 4), ((y0 + y) * sheet.width + x0) * 4);
  }
});
fs.writeFileSync(out, png.encode(sheet));
