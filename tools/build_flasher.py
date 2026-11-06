#!/usr/bin/env python3
"""Build the standalone web flasher: embeds the firmware images into
tools/flasher/DIVA_Bridge_ESP32_Flasher.template.html.

After `pio run -e esp32s3_devkit -e esp32s3_usb_otg`:

    python3 tools/build_flasher.py

writes dist/DIVA_Bridge_ESP32_Flasher.html - one file that works offline
except for loading esptool-js from jsDelivr.

Other build systems: pass the images per board,
    --board esp32s3_devkit:path/bootloader.bin,path/partitions.bin,path/firmware.bin
and --boot-app0 path/boot_app0.bin.
"""
import argparse, base64, glob, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "tools", "flasher", "DIVA_Bridge_ESP32_Flasher.template.html")
ENVS = ["esp32s3_devkit", "esp32s3_usb_otg"]
# ESP32-S3 Arduino layout; must match partitions.csv.
OFFSETS = {"bootloader": 0x0, "partitions": 0x8000, "boot_app0": 0xE000, "app": 0x10000}
CFG_OFFSET, APP_MAX = 0x310000, 0x300000


def find_boot_app0():
    pats = [os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin")]
    for p in pats:
        hits = sorted(glob.glob(p))
        if hits:
            return hits[-1]
    return None


def version():
    try:
        return subprocess.check_output(["git", "-C", ROOT, "describe", "--always", "--dirty", "--tags"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", action="append", default=[],
                    help="env:bootloader.bin,partitions.bin,firmware.bin (default: .pio/build/<env>)")
    ap.add_argument("--boot-app0", default=None)
    ap.add_argument("--version", default=None)
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "dist", "DIVA_Bridge_ESP32_Flasher.html"))
    args = ap.parse_args()

    sets = {}
    for b in args.board:
        env, paths = b.split(":", 1)
        sets[env] = paths.split(",")
    if not sets:
        for env in ENVS:
            d = os.path.join(ROOT, ".pio", "build", env)
            if os.path.exists(os.path.join(d, "firmware.bin")):
                sets[env] = [os.path.join(d, n) for n in ("bootloader.bin", "partitions.bin", "firmware.bin")]
    if not sets:
        sys.exit("no builds found - run `pio run` first, or pass --board")
    boot_app0 = args.boot_app0 or find_boot_app0()
    if not boot_app0:
        sys.exit("boot_app0.bin not found - pass --boot-app0")

    fw = {"version": args.version or version(), "boards": {}}
    for env, (bl, pt, app) in sets.items():
        parts = []
        for name, path in (("bootloader", bl), ("partitions", pt), ("boot_app0", boot_app0), ("app", app)):
            data = open(path, "rb").read()
            if name == "app" and len(data) > APP_MAX:
                sys.exit(f"{env}: firmware larger than the app partition")
            parts.append({"name": name, "offset": OFFSETS[name],
                          "data": base64.b64encode(data).decode()})
        # The partition table must contain divacfg where the page writes it.
        if b"divacfg" not in open(pt, "rb").read():
            sys.exit(f"{env}: partition table has no divacfg - build with partitions.csv")
        fw["boards"][env] = {"parts": parts}
        print(f"{env}: app {os.path.getsize(app)} bytes")

    html = open(TEMPLATE, encoding="utf-8").read()
    start, end = "/*FIRMWARE_JSON*/", "/*END_FIRMWARE_JSON*/"
    i, j = html.index(start) + len(start), html.index(end)
    html = html[:i] + json.dumps(fw, separators=(",", ":")) + html[j:]
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, "w", encoding="utf-8").write(html)
    print(f"wrote {args.out} ({os.path.getsize(args.out) // 1024} KB, firmware {fw['version']})")


if __name__ == "__main__":
    main()
