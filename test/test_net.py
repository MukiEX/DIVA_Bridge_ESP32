#!/usr/bin/env python3
"""Talk to a host build of the firmware's TCP session exactly like
DivallerBridge.dva / BridgeSlider do, and check the replies against what
diva_bridge.py would send.

    g++ -std=c++17 -pthread -Ihost_shim -o net_server net_server.cpp \\
        ../src/NetProtocol.cpp ../src/DivaState.cpp ../src/DivaProtocol.cpp
    python3 test_net.py /path/to/DIVA_Bridge_project/diva_bridge.py
"""
import importlib.util, os, socket, subprocess, sys, time

spec = importlib.util.spec_from_file_location("diva_bridge", sys.argv[1])
db = importlib.util.module_from_spec(spec); spec.loader.exec_module(db)
here = os.path.dirname(os.path.abspath(__file__))
port = 45811
srv = subprocess.Popen([os.path.join(here, "net_server"), str(port)], stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
time.sleep(0.3)

def ctl(cmd):
    srv.stdin.write(cmd + "\n"); srv.stdin.flush()
    return srv.stdout.readline().strip()

def rx(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b)); assert c, "closed"; b += c
    return b

def req(s, data, n_hdr):
    s.sendall(data); return rx(s, n_hdr)

try:
    s = socket.create_connection(("127.0.0.1", port))
    # No controller yet
    assert req(s, b"P", 1) == b"\x00"
    assert req(s, b"R\x84\x18", 2) == b"\x00\x00"
    assert req(s, b"X\x84\x1a", 2) == b"\x00\x00"
    assert req(s, b"W\x03\x03abc", 1) == b"\x00"
    assert ctl("plug") == "ack native=1", "first request should count as the handshake"
    # Plugged, nothing received yet -> idle packet
    assert req(s, b"P", 1) == b"\x01"
    h = req(s, b"R\x84\x18", 2); d = rx(s, h[1])
    assert h == b"\x01\x18" and d == db.HEADER + bytes(21), d.hex()
    # A HORI report: X (triangle) + L + hat up, slider leftmost + rightmost cell
    report = bytes([0x08 | 0x10, 0x00, 0x00, 0x80 ^ 0x01, 0x80, 0x80, 0x80 ^ 0x80, 0x00])
    ctl(report.hex())
    want_pkt, want_ext = db.HoriProfile().to_divaller(report)
    h = req(s, b"R\x84\x18", 2); d = rx(s, h[1])
    assert d == want_pkt[:24], (d.hex(), want_pkt.hex())
    h = req(s, b"X\x84\x1a", 2); d = rx(s, h[1])
    assert d == want_pkt[:24] + want_ext.to_bytes(2, "little"), d.hex()
    # 'R' with a short max, as the protocol allows
    h = req(s, b"R\x84\x05", 2); d = rx(s, h[1]); assert d == want_pkt[:5]
    # LEDs accepted while plugged
    dla = db.LED_HEADER + bytes(97)
    assert req(s, b"W\x03" + bytes([100]) + dla, 1) == b"\x01"
    # Second client also counts; leaving drops the count
    s2 = socket.create_connection(("127.0.0.1", port)); assert req(s2, b"P", 1) == b"\x01"
    assert ctl("noop") == "ack native=2"
    s2.close(); time.sleep(0.2); assert ctl("noop") == "ack native=1"
    # Unknown opcode drops the client
    s3 = socket.create_connection(("127.0.0.1", port)); s3.sendall(b"Z"); time.sleep(0.2)
    assert s3.recv(1) == b""
    # Unplug
    ctl("unplug"); assert req(s, b"P", 1) == b"\x00"
    s.close(); time.sleep(0.2); assert ctl("noop") == "ack native=0"
    print("ALL OK")
finally:
    srv.kill()
