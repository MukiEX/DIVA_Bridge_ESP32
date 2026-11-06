#!/usr/bin/env python3
"""Fuzz DivaProtocol.cpp against the reference translators in diva_bridge.py.

    g++ -std=c++17 -O1 -o protocol_cli protocol_cli.cpp ../src/DivaProtocol.cpp
    python3 test_protocol.py /path/to/DIVA_Bridge_project/diva_bridge.py
"""
import importlib.util, os, random, subprocess, sys

ref_path = sys.argv[1]
spec = importlib.util.spec_from_file_location("diva_bridge", ref_path)
db = importlib.util.module_from_spec(spec)
spec.loader.exec_module(db)

KINDS = {1: db.DivallerProfile, 2: db.ZhouSensorProfile, 3: db.HoriProfile, 4: db.HoriPs4Profile}
NAMES = ["fn", "square", "circle", "triangle", "cross", "l3", "l2", "l1"]
rng = random.Random(1234)
cases, expect = [], []

def rnd(n):
    return bytes(rng.getrandbits(8) for _ in range(n))

for _ in range(4000):
    kind = rng.choice(list(KINDS)); sr = rng.randint(0, 1); lr = rng.randint(0, 1)
    prof = KINDS[kind](bool(sr), bool(lr))
    n = rng.choice([prof.min_in - 1, prof.min_in, 8, 24, 27, 64]) if prof.min_in > 1 else rng.choice([24, 32, 64])
    data = bytearray(rnd(max(n, 1)))
    if kind == 4 and rng.random() < 0.8:
        data[0] = 1
    data = bytes(data)
    cases.append(f"in {kind} {sr} {lr} {data.hex()}")
    if len(data) < prof.min_in:
        expect.append("skip"); continue
    r = prof.to_divaller(data)
    if r is None:
        expect.append("skip"); continue
    pkt, ext = r
    expect.append(f"{bytes(pkt)[:64].hex()} {ext}")

for _ in range(1000):
    kind = rng.choice(list(KINDS)); sr = rng.randint(0, 1); lr = rng.randint(0, 1)
    prof = KINDS[kind](bool(sr), bool(lr))
    dla = db.LED_HEADER + rnd(97) if rng.random() < 0.9 else rnd(100)
    cases.append(f"led {kind} {sr} {lr} {dla.hex()}")
    out = prof.from_divaller_leds(dla)
    expect.append("none" if out is None else bytes(out).hex())

for _ in range(1000):
    pkt = db.HEADER + rnd(21)
    cases.append(f"dec 1 0 0 {pkt.hex()}")
    buttons, sensors = db.decode(pkt)
    pressed = sum(1 << i for i, n in enumerate(NAMES) if buttons[n])
    mask = sum(1 << i for i, s in enumerate(sensors) if s)
    expect.append(f"{pressed} {mask}")

cli = os.path.join(os.path.dirname(os.path.abspath(__file__)), "protocol_cli")
out = subprocess.run([cli], input="\n".join(cases) + "\n", capture_output=True, text=True, check=True).stdout.split("\n")
bad = 0
for c, e, g in zip(cases, expect, out):
    if e != g:
        bad += 1
        if bad <= 10:
            print("MISMATCH", c[:60], "\n  python:", e, "\n  c++:   ", g)
print(f"{len(cases)} cases, {bad} mismatches")
sys.exit(1 if bad else 0)
