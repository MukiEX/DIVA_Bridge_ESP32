#!/usr/bin/env python3
"""Drive web/index.html (the on-device page) against a mock of the board's
HTTP API, for each controller type, and check the LED and settings calls.
Screenshots go to the directory given as the second argument.

    python3 test_webui.py ../web/index.html /tmp/shots     (needs Playwright)
"""
import json, os, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from playwright.sync_api import sync_playwright

PAGE = open(sys.argv[1], "rb").read()
OUT = sys.argv[2]; os.makedirs(OUT, exist_ok=True)
CTL = {  # name, kind, vid, pid
    "divaller": ("Divaller", 1, "0E8F", "2213"),
    "zhou": ("ZhouSensor HDD", 2, "1973", "8000"),
    "hori": ("HORI NSW-230", 3, "0F0D", "00FB"),
    "ps4": ("HORI PS4-161", 4, "0F0D", "013C"),
    "none": ("", 0, "0000", "0000"),
}
state = {"ctl": "divaller", "reports": 0}
posts = {"leds": [], "settings": []}
settings = {"ssid": "Home Net", "pass": "", "name": "DIVA Bridge", "port": "45710", "radio": "both",
            "slider": "motion", "slider_reverse": "0", "led_reverse": "0", "leds": "1", "xbox": "seriesx",
            "otg_power": "usb", "pass_set": True}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == "/":
            return self.send(200, PAGE, "text/html; charset=utf-8")
        if self.path == "/api/state":
            name, kind, vid, pid = CTL[state["ctl"]]
            state["reports"] += 30
            # triangle + fn pressed, cells 0-3 and 28-31 touched, ext: up + LB
            pressed = (1 << 3) | (1 << 0)
            body = {"present": kind != 0, "kind": kind, "vid": vid, "pid": pid, "pressed": pressed,
                    "sensors": 0xF000000F, "ext": 1 | 16, "pkt": "42565a" + "00" * 21,
                    "reports": state["reports"], "native": 1, "tcp": 1, "ble": 1, "rssi": -48,
                    "port": 45710, "uptime": 3725, "heap": 180000, "radio": "both", "otg_board": False,
                    "name": name, "ip": "192.168.1.42", "host": "diva-bridge", "ssid": "Home Net",
                    "btname": "DIVA Bridge", "fw": "Nov  6 2026 20:00:00"}
            return self.send(200, json.dumps(body))
        if self.path == "/api/settings":
            return self.send(200, json.dumps(settings))
        self.send(404, "nf", "text/plain")
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        if self.path == "/api/leds":
            posts["leds"].append(body); return self.send(200, '{"ok":true}')
        if self.path == "/api/settings":
            posts["settings"].append(body); return self.send(200, '{"ok":true}')
        self.send(404, "nf", "text/plain")

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
url = f"http://127.0.0.1:{srv.server_address[1]}/"

with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page(viewport={"width": 1000, "height": 1000})
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(url)
    for key in CTL:
        state["ctl"] = key
        pg.wait_for_timeout(400)
        assert not errs, errs
        chip = pg.inner_text("#c-ctl")
        if key == "none":
            assert chip == "No controller", chip
        else:
            assert CTL[key][0] in chip, chip
        assert pg.is_hidden("#ledpanel") == (key not in ("divaller", "zhou")), key
        pg.locator("#cv").screenshot(path=os.path.join(OUT, f"tester_{key}.png"))
    assert "Xbox pad: muted" in pg.inner_text("#c-pad")
    assert pg.is_visible("#nativebanner")
    assert "reports/s" in pg.inner_text("#c-rate")
    # LEDs
    state["ctl"] = "divaller"; pg.wait_for_timeout(300)
    pg.click("[data-led=rainbow]"); pg.wait_for_timeout(500)
    assert len(posts["leds"]) >= 3, len(posts["leds"])
    pkt = bytes.fromhex(posts["leds"][-1]); assert len(pkt) == 100 and pkt[:3] == b"DLa" and pkt[3] == 0
    pg.click("[data-led=touch]"); pg.wait_for_timeout(300)
    pkt = bytes.fromhex(posts["leds"][-1])
    assert pkt[4 + 3 * 31:4 + 3 * 31 + 3] == b"\xff\xff\xff"   # sensor 0 touched -> white
    assert pkt[4 + 3 * 15:4 + 3 * 15 + 3] == bytes((40, 0, 60))  # sensor 16 idle (G,R,B)
    pg.click("[data-led=off]"); pg.wait_for_timeout(400); n = len(posts["leds"]); pg.wait_for_timeout(300)
    assert bytes.fromhex(posts["leds"][-1])[4:] == bytes(96) and len(posts["leds"]) == n
    # Status tab
    pg.click("[data-tab=status]")
    assert "host=192.168.1.42" in pg.inner_text("#ini") and "45710" in pg.inner_text("#toml")
    pg.screenshot(path=os.path.join(OUT, "status.png"), full_page=True)
    # Settings tab
    pg.click("[data-tab=settings]"); pg.wait_for_timeout(300)
    assert pg.input_value("#ssid") == "Home Net" and pg.get_attribute("#pass", "placeholder") == "unchanged (set)"
    assert pg.is_hidden("#otg_power")
    pg.fill("#port", "80"); pg.click("#save"); assert "not 80" in pg.inner_text("#saveinfo")
    pg.fill("#port", "45711"); pg.select_option("#slider", "position"); pg.check("#unpair")
    pg.click("#save"); pg.wait_for_timeout(300)
    body = posts["settings"][-1]
    assert "port=45711\n" in body and "slider=position\n" in body and "unpair=1\n" in body, body
    assert "pass=" not in body, "unchanged password must not be sent"
    pg.screenshot(path=os.path.join(OUT, "settings.png"), full_page=True)
    # phone width: no horizontal scroll
    pg2 = b.new_page(viewport={"width": 390, "height": 800}); pg2.goto(url); pg2.wait_for_timeout(400)
    assert pg2.evaluate("document.documentElement.scrollWidth") <= 390
    pg2.screenshot(path=os.path.join(OUT, "phone.png"), full_page=True)
    assert not errs, errs
    b.close()
print("ALL OK")
