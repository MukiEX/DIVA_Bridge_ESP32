#!/usr/bin/env python3
"""The DIVA Bridge relay (diva_bridge.py --device ble) against a simulated
ESP32: a fake `bleak` that speaks the GATT protocol (handshake, 1 s heartbeat
at protocol 2, BYE). Checks that the relay

  - handshakes and reads the controller,
  - handshakes again when a game connects (a fresh stream per game instance),
  - notices a stalled stream (no heartbeat) and handshakes again, then
    reconnects if that doesn't help,
  - says BYE when it's closed.

    python3 test_relay_ble.py /path/to/DIVA_Bridge_project/diva_bridge.py
"""
import asyncio, importlib.util, socket, sys, threading, time, types

CONTROL = "5e1a0001-d1ba-4b1d-9e00-000000d1ba00"


class FakeEsp:
    """Shared state of the simulated board (one per test run)."""
    def __init__(self):
        self.writes = []          # (char, bytes)
        self.handshakes = 0
        self.connects = 0
        self.stalled = False      # True: stop all notifications (a wedged stream)
        self.dead_after_handshake = False   # stall even after a new handshake
        self.input_cb = None
        self.status_cb = None
        self.natives = 0

    def status(self):
        return bytes((2, 1, 1, self.natives, 0x8F, 0x0E, 0x13, 0x22))   # Divaller 0E8F:2213

    def packet(self):
        return bytes((0x42, 0x56, 0x5A)) + bytes(21) + bytes(4)


ESP = FakeEsp()


class BleakClient:
    def __init__(self, device):
        self.is_connected = False
        self.mtu_size = 185
        self._task = None

    async def __aenter__(self):
        ESP.connects += 1
        self.is_connected = True
        self._task = asyncio.create_task(self._heartbeat())
        return self

    async def __aexit__(self, *a):
        self.is_connected = False
        self._task.cancel()

    async def _heartbeat(self):
        while True:
            await asyncio.sleep(0.2)   # the real thing: every 1 s while idle; faster here
            if ESP.input_cb and ESP.natives and not ESP.stalled:
                ESP.input_cb(None, bytearray(ESP.packet()))

    async def start_notify(self, char, cb):
        if char == CONTROL:
            ESP.status_cb = cb
        else:
            ESP.input_cb = cb

    async def stop_notify(self, char):
        if char != CONTROL:
            ESP.input_cb = None

    async def write_gatt_char(self, char, data, response=False):
        data = bytes(data)
        ESP.writes.append((char, data))
        if char == CONTROL and data.startswith(b"DIVA"):
            ESP.handshakes += 1
            ESP.natives = 1
            ESP.stalled = ESP.dead_after_handshake   # a new handshake restarts the stream
        elif char == CONTROL and data.startswith(b"BYE"):
            ESP.natives = 0

    async def read_gatt_char(self, char):
        return bytearray(ESP.status() if char == CONTROL else ESP.packet())


class BleakScanner:
    @staticmethod
    async def find_device_by_filter(f, timeout=10.0):
        return types.SimpleNamespace(name="DIVA Bridge", address="AA:BB:CC:DD:EE:FF")

    @staticmethod
    async def find_device_by_address(a, timeout=10.0):
        return await BleakScanner.find_device_by_filter(None)


sys.modules["bleak"] = types.SimpleNamespace(BleakClient=BleakClient, BleakScanner=BleakScanner)

spec = importlib.util.spec_from_file_location("diva_bridge", sys.argv[1])
db = importlib.util.module_from_spec(spec)
spec.loader.exec_module(db)
db.BLE_HEARTBEAT_TIMEOUT = 1.0   # quicker for the test
logs = []
_orig_log = db.log
db.log = lambda m: (logs.append(m), _orig_log(m))


def wait_for(cond, timeout=8.0, what=""):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + what + "\n" + "\n".join(logs[-15:]))


dev = db.BleBridgeDevice(None, "DIVA Bridge", False, True, 20)
wait_for(lambda: dev.present and ESP.handshakes == 1, what="first handshake")
assert dev.read(24)[:3] == b"BVZ"
print("handshake OK")

# A game connects over TCP: the relay restarts the stream.
srv = db.Server(("127.0.0.1", 0), db.make_handler(dev))
threading.Thread(target=srv.serve_forever, daemon=True).start()
time.sleep(2.2)   # past the 2 s re-handshake guard
s = socket.create_connection(srv.server_address)
s.sendall(b"P"); assert s.recv(1) == b"\x01"
wait_for(lambda: ESP.handshakes == 2, what="handshake on game connect")
s.close()
print("game connect -> new handshake OK")

# Wedged stream: notifications stop. The relay handshakes again and recovers.
ESP.stalled = True
wait_for(lambda: ESP.handshakes == 3, what="handshake after a silent stream")
assert any("handshaking again" in m for m in logs)
t = dev._last_notif; time.sleep(0.6); assert dev._last_notif > t, "stream should be back"
print("stall -> new handshake -> recovered OK")

# Still dead after a new handshake: drop the link and reconnect.
c0 = ESP.connects
ESP.dead_after_handshake = True; ESP.stalled = True
wait_for(lambda: ESP.connects > c0, timeout=10, what="reconnect")
ESP.dead_after_handshake = False; ESP.stalled = False
wait_for(lambda: dev.present, what="controller back after reconnect")
print("still dead -> reconnect OK")

# Closing says goodbye.
dev.close()
assert any(c == CONTROL and d == b"BYE" for c, d in ESP.writes), ESP.writes
assert ESP.natives == 0
print("close -> BYE OK")
print("ALL OK")
