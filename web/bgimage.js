// ---- Background image for screen boards (shared by web/index.html and the
// setup page; tools/gen_webpage.py and tools/build_setup_page.py inline it).
// Cover-crops the picture to the screen, lets you drag to pan, dims it, and
// packs it the way src/boards/WatchDisplay.cpp reads the "bgimage" partition:
// "DVBG", u16 width, u16 height, u32 byte length, RGB565 little-endian.
const BG_W = 410, BG_H = 502;
const BGIMG = { img: null, panX: 0.5, panY: 0.5, dim: 0.25 };

function bgDrawPicture(ctx, w, h) {
  const img = BGIMG.img;
  if (!img) {
    const g = ctx.createLinearGradient(0, 0, 0, h);
    g.addColorStop(0, "#123a48"); g.addColorStop(1, "#0b1220");
    ctx.fillStyle = g; ctx.fillRect(0, 0, w, h);
    return;
  }
  const s = Math.max(w / img.width, h / img.height);      // cover
  const dw = img.width * s, dh = img.height * s;
  ctx.drawImage(img, -(dw - w) * BGIMG.panX, -(dh - h) * BGIMG.panY, dw, dh);
  if (BGIMG.dim > 0) { ctx.fillStyle = `rgba(0,0,0,${BGIMG.dim})`; ctx.fillRect(0, 0, w, h); }
}

/** Preview: the picture plus the watch screen's panels/text, to scale. */
function bgPreview(canvas) {
  const ctx = canvas.getContext("2d"), k = canvas.width / BG_W;
  ctx.save(); bgDrawPicture(ctx, canvas.width, canvas.height); ctx.restore();
  ctx.save(); ctx.scale(k, k);
  const shade = (x, y, w, h) => { ctx.fillStyle = "rgba(0,0,0,0.75)"; ctx.fillRect(x, y, w, h); };
  const t = (x, y, s, c, f, a) => { ctx.fillStyle = c; ctx.font = f; ctx.textAlign = a || "left"; ctx.fillText(s, x, y); };
  shade(0, 0, BG_W, 64); t(20, 42, "DIVA Bridge", "#ececf0", "bold 30px Helvetica, Arial, sans-serif"); t(390, 40, "87%", "#9a9ca8", "bold 18px Helvetica, Arial", "right");
  shade(14, 78, BG_W - 28, 196); t(28, 108, "Divaller", "#ececf0", "bold 18px Helvetica, Arial");
  for (let i = 0; i < 32; i++) { ctx.fillStyle = i >= 12 && i <= 14 ? "#3fd7ea" : "#1e2a30"; ctx.fillRect(28 + i * 354 / 32 + 1, 128, 354 / 32 - 2, 34); }
  ["#4fd06a", "#e45dd0", "#5a8cff", "#f05050"].forEach((c, i) => { ctx.strokeStyle = c; ctx.lineWidth = 3; ctx.beginPath(); ctx.arc(72 + i * 89, 220, 35, 0, 7); ctx.stroke(); });
  shade(14, 288, BG_W - 28, 182);
  [["Wi-Fi", "192.168.1.120"], ["Games", "1 connected"], ["Xbox pad", "muted (game)"], ["USB", "763 reports/s"]].forEach(([a, b], i) => {
    t(28, 318 + i * 38, a, "#9a9ca8", "15px Helvetica, Arial"); t(132, 318 + i * 38, b, "#ececf0", "bold 18px Helvetica, Arial"); });
  shade(0, BG_H - 26, BG_W, 26);
  ctx.restore();
}

/** The partition blob for the current picture. */
function bgEncode() {
  const c = document.createElement("canvas"); c.width = BG_W; c.height = BG_H;
  const ctx = c.getContext("2d"); bgDrawPicture(ctx, BG_W, BG_H);
  const px = ctx.getImageData(0, 0, BG_W, BG_H).data;
  const out = new Uint8Array(12 + BG_W * BG_H * 2);
  out.set([0x44, 0x56, 0x42, 0x47]);                                     // "DVBG"
  const dv = new DataView(out.buffer);
  dv.setUint16(4, BG_W, true); dv.setUint16(6, BG_H, true); dv.setUint32(8, BG_W * BG_H * 2, true);
  for (let i = 0, o = 12; i < BG_W * BG_H; i++, o += 2) {
    const r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
    dv.setUint16(o, (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3, true);
  }
  return out;
}

/** Wire up a file input, preview canvas (drag to pan) and dim slider. */
function bgAttach(fileInput, canvas, dimInput, onChange) {
  const redraw = () => { bgPreview(canvas); onChange && onChange(); };
  fileInput.onchange = () => {
    const f = fileInput.files[0]; if (!f) return;
    const img = new Image();
    img.onload = () => { BGIMG.img = img; BGIMG.panX = BGIMG.panY = 0.5; redraw(); URL.revokeObjectURL(img.src); };
    img.src = URL.createObjectURL(f);
  };
  dimInput.oninput = () => { BGIMG.dim = dimInput.value / 100; redraw(); };
  let drag = null;
  canvas.onpointerdown = (e) => { if (BGIMG.img) { drag = { x: e.clientX, y: e.clientY, px: BGIMG.panX, py: BGIMG.panY }; canvas.setPointerCapture(e.pointerId); } };
  canvas.onpointermove = (e) => {
    if (!drag) return;
    const r = canvas.getBoundingClientRect(), img = BGIMG.img;
    const s = Math.max(BG_W / img.width, BG_H / img.height), k = r.width / BG_W;
    const spareX = img.width * s - BG_W, spareY = img.height * s - BG_H;
    if (spareX > 0) BGIMG.panX = Math.min(1, Math.max(0, drag.px - (e.clientX - drag.x) / k / spareX));
    if (spareY > 0) BGIMG.panY = Math.min(1, Math.max(0, drag.py - (e.clientY - drag.y) / k / spareY));
    redraw();
  };
  canvas.onpointerup = canvas.onpointercancel = () => { drag = null; };
  BGIMG.dim = dimInput.value / 100;
  redraw();
}
