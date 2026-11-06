#!/usr/bin/env python3
"""End-to-end test of the web flasher page (dist/DIVA_Bridge_ESP32_Flasher.html)
against a simulated ESP32-S3: esptool-js is replaced with a fake that keeps a
flash image in memory. Checks every image lands at the right offset and the
settings blob round-trips through the firmware's own decoder (config_cli).

    g++ -std=c++17 -o config_cli config_cli.cpp ../src/ConfigBlob.cpp
    python3 test_flasher.py ../dist/DIVA_Bridge_ESP32_Flasher.html
(needs Python Playwright + Chromium)
"""
import base64, json, os, re, subprocess, sys, tempfile
from playwright.sync_api import sync_playwright

page_path = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
cli = os.path.join(here, "config_cli")
html = open(page_path, encoding="utf-8").read()
fw = json.loads(re.search(r"/\*FIRMWARE_JSON\*/(.*?)/\*END_FIRMWARE_JSON\*/", html, re.S).group(1))

FAKE = r"""
export class Transport { constructor(port) { this.port = port; } async disconnect() { window.__fake.disconnects++; } }
export class ESPLoader {
  constructor(o) { this.o = o; window.__fake.loader = this; }
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

def decode(blob_bytes):
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(blob_bytes)
    out = subprocess.run([cli, "decode", t.name], capture_output=True, text=True).stdout
    os.unlink(t.name)
    if out.startswith("INVALID"):
        return None
    serial, _, payload = out.partition("\n")
    kv = dict(l.split("=", 1) for l in payload.splitlines() if "=" in l)
    return int(serial), kv

def flash(pg, addr, n):
    return bytes(pg.evaluate(f"Array.from(window.__fake.flash.slice({addr}, {addr + n}))"))

with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page(viewport={"width": 900, "height": 1400})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.route(re.compile(r".*esptool-js.*"), lambda r: r.fulfill(body=FAKE, content_type="application/javascript"))
    pg.add_init_script("""
      window.__fake = { flash: new Uint8Array(0x400000).fill(0xFF), writes: [], resets: [], disconnects: 0 };
      Object.defineProperty(navigator, 'serial', { value: { requestPort: async () => ({}) } });
    """)
    pg.goto("file://" + page_path)
    pg.wait_for_function("document.querySelectorAll('.board').length > 0")
    assert not errs, errs

    # 1. Fresh board: connect reads nothing, install firmware + settings
    pg.click("#connect"); pg.wait_for_selector("#install:not([disabled])")
    assert "No settings" in pg.inner_text("#readinfo")
    pg.fill("#ssid", "Home Net 5G"); pg.fill("#pass", "p@ss word=1")
    pg.select_option("#radio", "wifi"); pg.check("#led_reverse"); pg.check("#unpair")
    pg.click("#install"); pg.wait_for_function("window.__fake.resets.length === 1")
    parts = fw["boards"]["esp32s3_devkit"]["parts"]
    for part in parts:
        data = base64.b64decode(part["data"])
        assert flash(pg, part["offset"], len(data)) == data, part["name"]
    serial1, kv = decode(flash(pg, 0x310000, 0x1000))
    assert kv["ssid"] == "Home Net 5G" and kv["pass"] == "p@ss word=1", kv
    assert kv["radio"] == "wifi" and kv["led_reverse"] == "1" and kv["unpair"] == "1", kv
    assert kv["port"] == "45710" and kv["name"] == "DIVA Bridge", kv
    assert pg.evaluate("window.__fake.resets[0]") == "hard_reset"
    print("install OK, serial", serial1)

    # 2. Board rewrote its own blob (as the firmware's mirror does): reconnect + read
    with tempfile.TemporaryDirectory() as d:
        pl = os.path.join(d, "p"); out = os.path.join(d, "o")
        open(pl, "w").write("ssid=Home Net 5G\npass=p@ss word=1\nname=DIVA Bridge\nport=45710\nradio=wifi\n"
                            "slider=position\nslider_reverse=0\nled_reverse=1\nleds=0\nxbox=ones\notg_power=usb\n")
        subprocess.run([cli, "encode", str(serial1), pl, out], check=True)
        blob = list(open(out, "rb").read())
    pg.evaluate(f"window.__fake.flash.set({json.dumps(blob)}, 0x310000)")
    pg.click("#connect"); pg.wait_for_selector("#writecfg:not([disabled])")
    assert pg.input_value("#slider") == "position" and pg.input_value("#xbox") == "ones"
    assert not pg.is_checked("#leds") and pg.is_checked("#led_reverse") and not pg.is_checked("#unpair")
    assert pg.input_value("#pass") == "p@ss word=1"

    # 3. Settings only: only the 4 KB blob is written, with a new serial
    pg.fill("#ssid", "Other"); n_before = pg.evaluate("window.__fake.writes.length")
    pg.click("#writecfg"); pg.wait_for_function(f"window.__fake.writes.length === {n_before + 1}")
    w = pg.evaluate("window.__fake.writes.at(-1)")
    assert w == [{"address": 0x310000, "len": 0x1000}], w
    serial2, kv = decode(flash(pg, 0x310000, 0x1000))
    assert serial2 != serial1 and kv["ssid"] == "Other" and kv["slider"] == "position", (serial2, kv)
    print("settings-only OK")

    # 4. Firmware update keeps settings (no blob written)
    pg.click("#connect"); pg.wait_for_selector("#update:not([disabled])")
    pg.click("#update"); pg.wait_for_function(f"window.__fake.writes.length === {n_before + 2}")
    w = pg.evaluate("window.__fake.writes.at(-1)")
    assert 0x310000 not in [x["address"] for x in w] and len(w) == 4, w
    assert decode(flash(pg, 0x310000, 0x1000))[0] == serial2
    print("update OK")

    # 5. OTG board selection uses its own images
    pg.click("#connect"); pg.wait_for_selector("#install:not([disabled])")
    pg.click("text=Espressif ESP32-S3-USB-OTG"); assert pg.is_visible("#otg_power")
    pg.select_option("#otg_power", "battery")
    pg.click("#install"); pg.wait_for_function(f"window.__fake.writes.length === {n_before + 3}")
    app = base64.b64decode(fw["boards"]["esp32s3_usb_otg"]["parts"][3]["data"])
    assert flash(pg, 0x10000, len(app)) == app
    assert decode(flash(pg, 0x310000, 0x1000))[1]["otg_power"] == "battery"
    print("otg board OK")

    # 6. Blob encoder vs firmware decoder on awkward text
    weird = "ssid=Café ☕ Net\npass=\nname=x\n"
    blob = pg.evaluate(f"Array.from(window.__blob.encodeBlob({json.dumps(weird)}, 4294967295))")
    s, kv = decode(bytes(blob))
    assert s == 4294967295 and kv["ssid"] == "Café ☕ Net" and kv["pass"] == "", (s, kv)
    assert not errs, errs
    pg.screenshot(path=os.path.join(tempfile.gettempdir(), "flasher.png"), full_page=True)
    b.close()
print("ALL OK")
