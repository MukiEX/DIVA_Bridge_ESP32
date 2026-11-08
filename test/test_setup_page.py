#!/usr/bin/env python3
"""End-to-end test of the setup page (dist/DIVA_Bridge_ESP32_Setup.html)
against a simulated ESP32-S3. esptool-js is replaced by a fake that keeps a
flash image in memory, and the serial port plays the firmware's boot log and
answers `status`. Checks images land at the right offsets, the settings blob
round-trips through the firmware's own decoder (config_cli), the console /
Wi-Fi report / USB tree, and mode switching on one port.

    g++ -std=c++17 -o config_cli config_cli.cpp ../src/ConfigBlob.cpp
    python3 test_setup_page.py ../dist/DIVA_Bridge_ESP32_Setup.html
(needs Python Playwright + Chromium)
"""
import base64, json, os, re, subprocess, sys, tempfile
from playwright.sync_api import sync_playwright

page_path = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
cli = os.path.join(here, "config_cli")
html = open(page_path, encoding="utf-8").read()
fw = json.loads(re.search(r"/\*FIRMWARE_JSON\*/(.*?)/\*END_FIRMWARE_JSON\*/", html, re.S).group(1))
boards = list(fw["boards"])

FAKE_ESPTOOL = r"""
export class Transport { constructor(port) { this.device = port; window.__fake.transports++; } async disconnect() { window.__fake.disconnects++; } }
export class ESPLoader {
  constructor(o) { this.o = o; }
  async main() { this.o.terminal.writeLine("fake: chip ESP32-S3"); return "ESP32-S3 (QFN56) (revision v0.2)"; }
  async readFlash(addr, size) { return window.__fake.flash.slice(addr, addr + size); }
  async writeFlash(opt) {
    const f = window.__fake;
    if (opt.eraseAll) f.flash.fill(0xFF);
    f.writes.push(opt.fileArray.map(x => ({ address: x.address, len: x.data.length })));
    opt.fileArray.forEach((x, i) => { f.flash.set(x.data, x.address); opt.reportProgress && opt.reportProgress(i, x.data.length, x.data.length); });
  }
  async after(m) { window.__fake.resets.push(m); }
}
"""

INIT = r"""
const NL = String.fromCharCode(13, 10);
window.__fake = { flash: new Uint8Array(0x400000).fill(0xFF), writes: [], resets: [], disconnects: 0,
  transports: 0, opens: 0, sent: [], consoleFails: false, bootLog: [], statusUsb: "[]" };
const port = {
  async open() {
    if (window.__fake.consoleFails) throw new Error("device lost");
    window.__fake.opens++;
    let ctl;
    this.readable = new ReadableStream({ start(c) { ctl = c; for (const l of window.__fake.bootLog) c.enqueue(new TextEncoder().encode(l + NL)); } });
    this.writable = new WritableStream({ write(chunk) {
      const t = new TextDecoder().decode(chunk); window.__fake.sent.push(t.trim());
      if (t.startsWith("status")) {
        const lines = ["--- status ---", "version=0.4.0", "controller=HORI NSW-230 (0F0D:00FB)", "reports=1234",
          "wifi=connected 192.168.1.42", "tcp_clients=0", "ble=1 connected", "native_clients=0", "free_heap=180000",
          "usb=" + window.__fake.statusUsb, "--- end ---"];
        for (const l of lines) ctl.enqueue(new TextEncoder().encode(l + NL));
      }
    } });
  },
  async setSignals() {},
  async close() {},
};
Object.defineProperty(navigator, "serial", { value: { requestPort: async () => { window.__fake.picks = (window.__fake.picks || 0) + 1; return port; } } });
"""

USB = json.dumps([
    {"addr": 1, "parent": 0, "port": 0, "vid": "1A40", "pid": "0101", "cls": 9, "speed": "full", "product": "USB 2.0 Hub", "mfr": "", "role": "hub"},
    {"addr": 2, "parent": 1, "port": 1, "vid": "0F0D", "pid": "00FB", "cls": 0, "speed": "full", "product": "HORI Controller", "mfr": "HORI", "role": "controller: HORI NSW-230"},
    {"addr": 3, "parent": 1, "port": 3, "vid": "046D", "pid": "C52B", "cls": 0, "speed": "full", "product": "Receiver", "mfr": "Logitech", "role": "not a supported controller"},
])
BOOT_OK = ["=== DIVA Bridge ESP32 v0.4.0 ===  (type 'help' for commands)", "[Net] connecting to 'Home Net 5G'...",
           "[Net] Wi-Fi connected: 192.168.1.42  (set DivallerBridge.ini host=192.168.1.42 or diva-bridge.local; tester at http://192.168.1.42/)"]


def decode(blob_bytes):
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(blob_bytes)
    out = subprocess.run([cli, "decode", t.name], capture_output=True, text=True).stdout
    os.unlink(t.name)
    if out.startswith("INVALID"):
        return None
    serial, _, payload = out.partition("\n")
    return int(serial), dict(l.split("=", 1) for l in payload.splitlines() if "=" in l)


def encode(serial, text):
    with tempfile.TemporaryDirectory() as d:
        pl, out = os.path.join(d, "p"), os.path.join(d, "o")
        open(pl, "w").write(text)
        subprocess.run([cli, "encode", str(serial), pl, out], check=True)
        return list(open(out, "rb").read())


with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page(viewport={"width": 900, "height": 1500})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.route(re.compile(r".*esptool-js.*"), lambda r: r.fulfill(body=FAKE_ESPTOOL, content_type="application/javascript"))
    pg.add_init_script(INIT)
    pg.goto("file://" + page_path)
    pg.wait_for_function("document.querySelectorAll('.board').length > 0")
    assert not errs, errs
    assert f"v{fw['version']}" in pg.inner_text("#fwver")
    fl = lambda a, n: bytes(pg.evaluate(f"Array.from(window.__fake.flash.slice({a}, {a + n}))"))
    js = lambda e: pg.evaluate(e)

    # 1. Fill settings, install from the Install tab without connecting first
    pg.click("[data-tab=settings]")
    pg.fill("#ssid", "Home Net 5G"); pg.fill("#pass", "p@ss word=1")
    assert "auto" in pg.eval_on_selector_all("#radio option", "els => els.map(e => e.value)")
    pg.select_option("#radio", "wifi"); pg.check("#led_reverse"); pg.check("#unpair")
    pg.click("[data-tab=install]")
    js(f"window.__fake.bootLog = {json.dumps(BOOT_OK)}; window.__fake.statusUsb = {json.dumps(USB)}")
    pg.click("#install")
    pg.wait_for_function("document.getElementById('wifiresult').textContent.includes('192.168.1.42')")
    for part in fw["boards"][boards[0]]["parts"]:
        data = base64.b64decode(part["data"])
        assert fl(part["offset"], len(data)) == data, part["name"]
    serial1, kv = decode(fl(0x310000, 0x1000))
    assert kv["ssid"] == "Home Net 5G" and kv["pass"] == "p@ss word=1" and kv["radio"] == "wifi", kv
    assert kv["led_reverse"] == "1" and kv["unpair"] == "1" and kv["name"] == "DIVA Bridge", kv
    assert js("window.__fake.resets") == ["hard_reset"]
    assert js("window.__fake.picks") == 1, "port picked once and reused"
    assert pg.inner_text("#conn").startswith("Console"), pg.inner_text("#conn")
    assert "http://192.168.1.42/" in pg.inner_html("#wifiresult")
    print("install OK | wifi:", pg.inner_text("#wifiresult").splitlines()[0])

    # 2. Console: status + USB tree arrived after Wi-Fi, version chip, commands
    pg.click("[data-tab=console]")
    pg.wait_for_function("document.getElementById('statustable').textContent.includes('HORI NSW-230')")
    tree = pg.inner_text("#usbtree")
    assert "USB 2.0 Hub" in tree and "HORI Controller" in tree and "port 3" in tree and "not a supported controller" in tree, tree
    assert "Board has v0.4.0" in pg.inner_text("#boardver")
    pg.fill("#cmd", "set pass secret"); pg.press("#cmd", "Enter"); pg.wait_for_timeout(200)
    assert "set pass secret" in js("window.__fake.sent") and "set pass secret" not in pg.inner_text("#log")
    js('window.__fake.statusUsb = "[]"'); pg.click("#refresh"); pg.wait_for_timeout(300)
    assert "Nothing attached" in pg.inner_text("#usbtree")
    pg.screenshot(path=os.path.join(tempfile.gettempdir(), "setup_console.png"), full_page=True)
    print("console OK")

    # 3. Settings only, from console mode: switches to flashing, blob only, wrong-password report
    js("""window.__fake.bootLog = ["[Net] connecting to 'Other'...", "[Net] Wi-Fi: can't join 'Other': wrong password (reason 15) - retrying"]""")
    pg.click("[data-tab=settings]")
    pg.fill("#ssid", "Other"); n = js("window.__fake.writes.length")
    pg.click("#writecfg")
    pg.wait_for_function("document.getElementById('wifiresult').textContent.includes('wrong password')")
    assert js("window.__fake.writes.at(-1)") == [{"address": 0x310000, "len": 0x1000}]
    serial2, kv = decode(fl(0x310000, 0x1000))
    assert serial2 != serial1 and kv["ssid"] == "Other", kv
    print("settings OK | wifi:", pg.inner_text("#wifiresult").splitlines()[0])

    # 4. Read from board: older firmware -> update offered; form filled
    old_cfg = "ssid=Other\npass=x\nname=Pad 2\nport=45710\nradio=both\nslider=position\nslider_reverse=0\nled_reverse=1\nleds=0\nxbox=ones\notg_power=usb\nfw=0.3.0\n"
    js(f"document.getElementById('readinfo').textContent = ''; window.__fake.flash.set({json.dumps(encode(serial2, old_cfg))}, 0x310000)")
    pg.click("#read"); pg.wait_for_function("document.getElementById('readinfo').textContent.includes('Loaded')")
    assert pg.input_value("#name") == "Pad 2" and pg.input_value("#slider") == "position" and not pg.is_checked("#leds")
    assert "update available" in pg.inner_text("#boardver"), pg.inner_text("#boardver")
    assert pg.inner_text("#conn").startswith("Flashing mode")

    # 5. Disconnect from flashing mode resets the board
    r0 = len(js("window.__fake.resets")); pg.click("#disconnect"); pg.wait_for_timeout(200)
    assert len(js("window.__fake.resets")) == r0 + 1 and pg.inner_text("#conn") == "Not connected"
    print("read / disconnect OK")

    # 6. Update keeps settings; single-USB style board: console can't reopen -> fallback text
    js("window.__fake.consoleFails = true")
    pg.click("[data-tab=install]"); n = js("window.__fake.writes.length")
    pg.click("#update"); pg.wait_for_function(f"window.__fake.writes.length === {n + 1}")
    w = js("window.__fake.writes.at(-1)")
    assert 0x310000 not in [x["address"] for x in w] and len(w) == 4, w
    assert decode(fl(0x310000, 0x1000))[0] == serial2
    pg.wait_for_function("document.getElementById('wifiresult').textContent.includes('diva-bridge.local')")
    print("update OK | fallback:", pg.inner_text("#wifiresult")[:60])

    # 7. Second board image, if built in
    if len(boards) > 1:
        js("window.__fake.consoleFails = false")
        pg.click("text=Espressif ESP32-S3-USB-OTG"); pg.click("[data-tab=settings]")
        assert pg.is_visible("#otg_power"); pg.select_option("#otg_power", "battery")
        pg.click("[data-tab=install]"); n = js("window.__fake.writes.length")
        pg.click("#install"); pg.wait_for_function(f"window.__fake.writes.length === {n + 1}")
        app = base64.b64decode(fw["boards"]["esp32s3_usb_otg"]["parts"][3]["data"])
        assert fl(0x10000, len(app)) == app and decode(fl(0x310000, 0x1000))[1]["otg_power"] == "battery"
        print("otg board OK")

    # 7b. Watch board: screen settings, background picture (scaled/cropped), no console
    if "esp32s3_amoled_206" in boards:
        from PIL import Image
        img_path = os.path.join(tempfile.gettempdir(), "bg_test.png")
        im = Image.new("RGB", (1200, 600), (255, 0, 0))          # wide: left half red, right half blue
        im.paste((0, 0, 255), (600, 0, 1200, 600)); im.save(img_path)
        pg.click("text=Waveshare ESP32-S3 AMOLED"); pg.click("[data-tab=settings]")
        assert pg.is_visible("#brightness") and pg.is_visible("#bgprev") and not pg.is_visible("#otg_power")
        pg.select_option("#screen_timeout", "300")
        pg.set_input_files("#bgfile", img_path)
        pg.wait_for_function("!document.getElementById('bgwrite').disabled")
        pg.fill("#bgdim", "0"); pg.dispatch_event("#bgdim", "input")
        pg.click("[data-tab=console]")
        assert pg.is_visible("text=No serial console on this board") and not pg.is_visible("#usbtree")
        pg.click("[data-tab=install]"); n = js("window.__fake.writes.length")
        pg.click("#install"); pg.wait_for_function(f"window.__fake.writes.length === {n + 1}")
        w = js("window.__fake.writes.at(-1)")
        assert {"address": 0x320000, "len": 12 + 410 * 502 * 2} in w, w
        hdr = fl(0x320000, 12)
        assert hdr[:4] == b"DVBG" and int.from_bytes(hdr[4:6], "little") == 410 and int.from_bytes(hdr[6:8], "little") == 502
        px = lambda x, y: int.from_bytes(fl(0x320000 + 12 + (y * 410 + x) * 2, 2), "little")
        # 1200x600 covered onto 410x502 and centred: the middle column is the red/blue edge
        assert px(10, 250) == 0xF800 and px(400, 250) == 0x001F, (hex(px(10, 250)), hex(px(400, 250)))
        assert decode(fl(0x310000, 0x1000))[1]["screen_timeout"] == "300"
        pg.wait_for_function("document.getElementById('wifiresult').textContent.includes('Unplug the watch')")
        # picture only, then remove
        pg.click("[data-tab=settings]"); n = js("window.__fake.writes.length")
        pg.click("#bgwrite"); pg.wait_for_function(f"window.__fake.writes.length === {n + 1}")
        assert js("window.__fake.writes.at(-1)") == [{"address": 0x320000, "len": 12 + 410 * 502 * 2}]
        pg.click("#bgclear"); pg.wait_for_function(f"window.__fake.writes.length === {n + 2}")
        assert js("window.__fake.writes.at(-1)") == [{"address": 0x320000, "len": 0x1000}] and fl(0x320000, 4) == b"\xff" * 4
        pg.screenshot(path=os.path.join(tempfile.gettempdir(), "setup_watch.png"), full_page=True)
        pg.locator("#bgprev").screenshot(path=os.path.join(tempfile.gettempdir(), "preview_portrait_panels.png"))
        # rotation + outline backdrop: preview and picture follow, settings carry them
        pg.select_option("#rotation", "90"); pg.select_option("#panel_style", "outline")
        pg.fill("#panel_opacity", "60"); pg.dispatch_event("#panel_opacity", "input"); pg.fill("#panel_pad", "8"); pg.dispatch_event("#panel_pad", "input")
        assert js("[document.getElementById('bgprev').width, document.getElementById('bgprev').height]") == [502, 410]
        pg.locator("#bgprev").screenshot(path=os.path.join(tempfile.gettempdir(), "preview_landscape_outline.png"))
        n = js("window.__fake.writes.length"); pg.click("#bgwrite"); pg.wait_for_function(f"window.__fake.writes.length === {n + 1}")
        hdr = fl(0x320000, 12)
        assert int.from_bytes(hdr[4:6], "little") == 502 and int.from_bytes(hdr[6:8], "little") == 410, hdr
        pg.click("#writecfg"); pg.wait_for_function(f"window.__fake.writes.length === {n + 2}")
        kv = decode(fl(0x310000, 0x1000))[1]
        assert kv["rotation"] == "90" and kv["panel_style"] == "outline" and kv["panel_opacity"] == "60" and kv["panel_pad"] == "8", kv
        pg.select_option("#rotation", "0"); pg.select_option("#panel_style", "panels")
        print("watch board OK")

    # 8. Encoder vs firmware decoder on awkward text
    blob = js("Array.from(window.__blob.encodeBlob('ssid=Caf\\u00e9 \\u2615 Net\\npass=\\nname=x\\n', 4294967295))")
    s, kv = decode(bytes(blob))
    assert s == 4294967295 and kv["ssid"] == "Café ☕ Net" and kv["pass"] == "", (s, kv)
    assert not errs, errs
    pg.click("[data-tab=install]")
    pg.screenshot(path=os.path.join(tempfile.gettempdir(), "setup_install.png"), full_page=True)
    b.close()
print("ALL OK")
