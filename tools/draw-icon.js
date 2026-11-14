// Draws resources/icon.png and resources/icon~bw.png: a 25x25 timer ring, three quarters run, around a tick.
// usage: node tools/draw-icon.js
const fs = require('fs');
const path = require('path');
const png = require('./lib/png-diff');

const W = 25;
const RED = [170, 0, 0, 255];     // GColorDarkCandyAppleRed, the focus colour
const GREY = [170, 170, 170, 255];
const BLACK = [0, 0, 0, 255];

function build(bw) {
  const data = new Uint8Array(W * W * 4);  // transparent
  const px = (x, y, col) => { if (x >= 0 && y >= 0 && x < W && y < W) data.set(col, (y * W + x) * 4); };
  const c = (W - 1) / 2;
  // ring: the elapsed three quarters in the focus colour, the rest grey (black dots on black-and-white watches)
  for (let y = 0; y < W; y++) {
    for (let x = 0; x < W; x++) {
      const dx = x - c, dy = y - c, r = Math.sqrt(dx * dx + dy * dy);
      if (r < 9.2 || r > 12.2) continue;
      const left = dx < 0 && dy < 0;  // the quarter still to go
      if (!left) px(x, y, bw ? BLACK : RED);
      else if (!bw) px(x, y, GREY);
      else if ((x + y) % 2 === 0 && r > 10 && r < 11.6) px(x, y, BLACK);
    }
  }
  // tick, three pixels thick
  const stroke = (x0, y0, x1, y1) => {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0));
    for (let i = 0; i <= n; i++) {
      const x = Math.round(x0 + (x1 - x0) * i / n), y = Math.round(y0 + (y1 - y0) * i / n);
      for (let d = -1; d <= 1; d++) px(x, y + d, BLACK);
    }
  };
  stroke(7, 12, 10, 15);
  stroke(10, 15, 17, 8);
  return { width: W, height: W, data };
}

const out = path.join(__dirname, '..', 'resources');
fs.writeFileSync(path.join(out, 'icon.png'), png.encode(build(false)));
fs.writeFileSync(path.join(out, 'icon~bw.png'), png.encode(build(true)));
