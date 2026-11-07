// ---- Screen preview + background image for screen boards (shared by
// web/index.html and the setup page; tools/gen_webpage.py and
// tools/build_setup_page.py inline it).
// Cover-crops the picture to the screen, lets you drag to pan, dims it, and
// packs it the way src/boards/WatchDisplay.cpp reads the "bgimage" partition:
// "DVBG", u16 width, u16 height, u32 byte length, RGB565 little-endian.
// The picture is made at the screen's current orientation (rotation setting).
// The preview draws the watch UI over it with the same layout numbers as
// WatchDisplay.cpp, honouring rotation, panel opacity and panel style.
const BGIMG = { img: null, panX: 0.5, panY: 0.5, dim: 0.25, rotation: 0, opacity: 75, outline: false, pad: 6 };
const BG_LAYOUT = {   // keep in step with PORTRAIT / LANDSCAPE in WatchDisplay.cpp
  portrait:  { titleH: 64, titleBase: 44, cardY: 74, cardH: 200, nameY: 104, pillY: 86, sliderY: 124, sliderH: 34,
               faceY: 214, faceR: 34, extY: 266, statY: 286, statH: 180, cols: 1, rowStart: 314, rowH: 30, valueX: 128, footH: 26 },
  landscape: { titleH: 56, titleBase: 40, cardY: 64, cardH: 184, nameY: 92, pillY: 74, sliderY: 106, sliderH: 32,
               faceY: 186, faceR: 30, extY: 238, statY: 256, statH: 118, cols: 2, rowStart: 282, rowH: 30, valueX: 104, footH: 26 },
};
let bgCanvas = null, bgOnChange = null;

/** Logical screen size for the current rotation. */
function bgDims() { return BGIMG.rotation % 180 ? [502, 410] : [410, 502]; }

function bgDrawPicture(ctx, w, h) {
  const img = BGIMG.img;
  if (!img) {   // the firmware's default gradient, roughly
    const g = ctx.createLinearGradient(0, 0, 0, h);
    g.addColorStop(0, "#0a2834"); g.addColorStop(1, "#122030");
    ctx.fillStyle = g; ctx.fillRect(0, 0, w, h);
    const r = ctx.createRadialGradient(w * 0.75, h * 0.15, 0, w * 0.75, h * 0.15, Math.max(w, h) * 0.45);
    r.addColorStop(0, "rgba(20,90,100,0.9)"); r.addColorStop(1, "rgba(20,90,100,0)");
    ctx.fillStyle = r; ctx.fillRect(0, 0, w, h);
    return;
  }
  const s = Math.max(w / img.width, h / img.height);      // cover
  const dw = img.width * s, dh = img.height * s;
  ctx.drawImage(img, -(dw - w) * BGIMG.panX, -(dh - h) * BGIMG.panY, dw, dh);
  if (BGIMG.dim > 0) { ctx.fillStyle = `rgba(0,0,0,${BGIMG.dim})`; ctx.fillRect(0, 0, w, h); }
}

/** Preview: the picture plus the watch screen's UI, to scale. */
function bgPreview(canvas) {
  const [W, H] = bgDims();
  if (canvas.width !== W || canvas.height !== H) { canvas.width = W; canvas.height = H; }
  canvas.style.width = W / 2 + "px";
  const ctx = canvas.getContext("2d"), L = W > H ? BG_LAYOUT.landscape : BG_LAYOUT.portrait;
  ctx.save(); bgDrawPicture(ctx, W, H); ctx.restore();
  const alpha = BGIMG.opacity / 100, PAD = BGIMG.pad, outline = BGIMG.outline;
  const shade = (x, y, w, h) => { if (alpha > 0) { ctx.fillStyle = `rgba(0,0,0,${alpha})`; ctx.fillRect(x, y, w, h); } };
  const panel = (x, y, w, h) => { if (!outline) shade(x, y, w, h); };
  const item = (x, y, w, h, r = 2) => {
    if (!outline || alpha <= 0) return;
    const W2 = w + 2 * PAD, H2 = h + 2 * PAD;
    ctx.fillStyle = `rgba(0,0,0,${alpha})`; ctx.beginPath();
    ctx.roundRect(x - PAD, y - PAD, W2, H2, Math.min(r + PAD, W2 / 2, H2 / 2)); ctx.fill();
  };
  const FONTS = { B24: "bold 25px Helvetica, Arial, sans-serif", B14: "bold 18px Helvetica, Arial, sans-serif",
                  R12: "16px Helvetica, Arial, sans-serif", B10: "bold 13px Helvetica, Arial, sans-serif",
                  R10: "13px Helvetica, Arial, sans-serif" };
  const text = (x, y, s, color, font, align = 0, box = true) => {
    ctx.font = FONTS[font]; ctx.textAlign = "left"; ctx.textBaseline = "alphabetic";
    const m = ctx.measureText(s), w = m.width;
    if (align) x -= align > 0 ? w : w / 2;
    if (box) item(x, y - m.actualBoundingBoxAscent, w, m.actualBoundingBoxAscent + m.actualBoundingBoxDescent);
    ctx.fillStyle = color; ctx.fillText(s, x, y);
  };
  const TEXT = "#ececf0", DIM = "#9a9ca8", CYAN = "#3fd7ea", GREEN = "#4fd06a", AMBER = "#f2c84b", PINK = "#e45dd0", BLUE = "#5a8cff", RED = "#f05050";

  panel(0, 0, W, L.titleH);
  text(W / 2, L.titleBase, "DIVA BRIDGE", TEXT, "B24", -1);

  panel(14, L.cardY, W - 28, L.cardH);
  text(28, L.nameY, "Divaller", TEXT, "B14");
  ["L1", "L2", "L3", "FN"].forEach((t, i) => {
    const x = W - 28 - (4 - i) * 50, y = L.pillY, on = i === 3;
    item(x, y, 44, 24, 12);
    ctx.fillStyle = on ? AMBER : "#2a2c34"; ctx.beginPath(); ctx.roundRect(x, y, 44, 24, 12); ctx.fill();
    text(x + 22, y + 17, t, on ? "#000" : DIM, "B10", -1, false);
  });
  const sx = 28, sy = L.sliderY, sw = W - 56, sh = L.sliderH;
  item(sx - 4, sy - 4, sw + 8, sh + 8, 8);
  ctx.strokeStyle = "#2b6e78"; ctx.lineWidth = 1; ctx.beginPath(); ctx.roundRect(sx - 3.5, sy - 3.5, sw + 7, sh + 7, 8); ctx.stroke();
  for (let i = 0; i < 32; i++) {
    const x0 = sx + Math.floor(i * sw / 32), x1 = sx + Math.floor((i + 1) * sw / 32);
    ctx.fillStyle = i >= 12 && i <= 14 ? CYAN : i % 8 ? "#1e2a30" : "#26363d";
    ctx.fillRect(x0 + 1, sy, x1 - x0 - 2, sh);
  }
  const r = L.faceR, span = W - 56 - 2 * (r + 8);
  [GREEN, PINK, BLUE, RED].forEach((c, i) => {
    const cx = 28 + r + 8 + Math.floor(i * span / 3), cy = L.faceY;
    if (outline && alpha > 0) { ctx.fillStyle = `rgba(0,0,0,${alpha})`; ctx.beginPath(); ctx.arc(cx, cy, r + PAD, 0, 7); ctx.fill(); }
    if (i === 2) { ctx.fillStyle = c; ctx.beginPath(); ctx.arc(cx, cy, r, 0, 7); ctx.fill(); }
    ctx.strokeStyle = c; ctx.lineWidth = 3; ctx.beginPath(); ctx.arc(cx, cy, r - 1, 0, 7); ctx.stroke();
  });
  text(W / 2, L.extY, "Up LB", AMBER, "B10", -1);

  panel(14, L.statY, W - 28, L.statH);
  let col = 0, y = L.rowStart;
  const colW = Math.floor((W - 28) / L.cols);
  const row = (label, value, color, sub) => {
    const x = 28 + col * colW;
    text(x, y, label, DIM, "R12"); text(x + L.valueX - 28, y, value, color, "B14");
    if (sub) { y += 18; text(x + L.valueX - 28, y, sub, DIM, "R10"); }
    y += L.rowH;
  };
  row("Wi-Fi", "192.168.1.120", TEXT, "diva-bridge.local:45710");
  row("Games", "1 (1 Wi-Fi)", GREEN);
  if (L.cols > 1) { col = 1; y = L.rowStart; }
  row("Xbox pad", "muted (game)", DIM);
  row("USB", "763 reports/s", DIM);
  row("Battery", "87% charging", GREEN);

  panel(0, H - L.footH, W, L.footH);
  text(W / 2, H - 8, "v0.5.1  -  hold BOOT while plugging in to flash", DIM, "R10", -1);
}

/** The partition blob for the current picture, at the current orientation. */
function bgEncode() {
  const [W, H] = bgDims();
  const c = document.createElement("canvas"); c.width = W; c.height = H;
  const ctx = c.getContext("2d"); bgDrawPicture(ctx, W, H);
  const px = ctx.getImageData(0, 0, W, H).data;
  const out = new Uint8Array(12 + W * H * 2);
  out.set([0x44, 0x56, 0x42, 0x47]);                                     // "DVBG"
  const dv = new DataView(out.buffer);
  dv.setUint16(4, W, true); dv.setUint16(6, H, true); dv.setUint32(8, W * H * 2, true);
  for (let i = 0, o = 12; i < W * H; i++, o += 2) {
    const r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
    dv.setUint16(o, (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3, true);
  }
  return out;
}

/** Re-read rotation / panel opacity / panel style from the page's settings
 *  fields (ids rotation, panel_opacity, panel_style) and redraw. Call after
 *  the page fills the form from the board. */
function bgSync() {
  const v = (id) => { const el = document.getElementById(id); return el ? el.value : null; };
  if (v("rotation") !== null) BGIMG.rotation = Number(v("rotation")) || 0;
  if (v("panel_opacity") !== null) BGIMG.opacity = Number(v("panel_opacity"));
  if (v("panel_style") !== null) BGIMG.outline = v("panel_style") === "outline";
  if (v("panel_pad") !== null) BGIMG.pad = Number(v("panel_pad")) || 6;
  if (bgCanvas) { bgPreview(bgCanvas); bgOnChange && bgOnChange(); }
}

/** Wire up a file input, preview canvas (drag to pan), dim slider and the
 *  screen settings fields. */
function bgAttach(fileInput, canvas, dimInput, onChange) {
  bgCanvas = canvas; bgOnChange = onChange;
  const redraw = () => { bgPreview(canvas); onChange && onChange(); };
  fileInput.onchange = () => {
    const f = fileInput.files[0]; if (!f) return;
    const img = new Image();
    img.onload = () => { BGIMG.img = img; BGIMG.panX = BGIMG.panY = 0.5; redraw(); URL.revokeObjectURL(img.src); };
    img.src = URL.createObjectURL(f);
  };
  dimInput.oninput = () => { BGIMG.dim = dimInput.value / 100; redraw(); };
  for (const id of ["rotation", "panel_opacity", "panel_style", "panel_pad"]) {
    const el = document.getElementById(id);
    if (el) { el.addEventListener("input", bgSync); el.addEventListener("change", bgSync); }
  }
  let drag = null;
  canvas.onpointerdown = (e) => { if (BGIMG.img) { drag = { x: e.clientX, y: e.clientY, px: BGIMG.panX, py: BGIMG.panY }; canvas.setPointerCapture(e.pointerId); } };
  canvas.onpointermove = (e) => {
    if (!drag) return;
    const [W, H] = bgDims(), r = canvas.getBoundingClientRect(), img = BGIMG.img;
    const s = Math.max(W / img.width, H / img.height), k = r.width / W;
    const spareX = img.width * s - W, spareY = img.height * s - H;
    if (spareX > 0) BGIMG.panX = Math.min(1, Math.max(0, drag.px - (e.clientX - drag.x) / k / spareX));
    if (spareY > 0) BGIMG.panY = Math.min(1, Math.max(0, drag.py - (e.clientY - drag.y) / k / spareY));
    redraw();
  };
  canvas.onpointerup = canvas.onpointercancel = () => { drag = null; };
  BGIMG.dim = dimInput.value / 100;
  bgSync();
  redraw();
}
