# DIVA Bridge ESP32

Plug a Project DIVA slider controller into an ESP32-S3 and use it wirelessly:

```
                                   ┌──────────── Bluetooth: Xbox controller (any PC / Quest / phone)
Divaller / DIVA Pro /   USB   ┌────┴─────┐
HORI NSW-230 / PS4-161 ─────► │ ESP32-S3 ├─── Wi-Fi: DIVA Bridge TCP server  (DivallerBridge.dva, BridgeSlider,
   (powered hub)              └────┬─────┘                                      the Quest recomp - unchanged)
                                   └──────────── Bluetooth: DIVA Bridge service (diva_bridge.py --device ble)
```

- **Fallback: a Bluetooth Xbox controller.** Anything that pairs with it gets buttons, D-pad and the slider on the sticks.
- **Native mode: the real slider and LEDs.** The board speaks the same protocol as [`diva_bridge.py`](https://github.com/MukiEX/DIVA_Bridge_project), over Wi-Fi or Bluetooth. A client turns native mode on by *handshaking*: over Wi-Fi its first bridge request counts, and over Bluetooth it writes `DIVA`. From then on the Xbox pad goes quiet, so the game doesn't see the controller twice. When the last native client leaves, the pad comes back.

Fork of [KoStard/ESP32S3-USB-Keyboard-To-BLE](https://github.com/KoStard/ESP32S3-USB-Keyboard-To-BLE). The keyboard bridge was replaced with a raw USB host client, because the Divaller isn't a HID device and the controllers need LED output.

## Controllers

The controller tables are a straight port of `diva_bridge.py`. The test in `test/` checks them byte for byte against it.

| Controller | VID:PID | Mode | LEDs |
|---|---|---|---|
| DJ-DAO Divaller | 0E8F:2213 | firmware 2.4, PC Driver Mode (purple) | slider + buttons |
| ZhouSensor / DIVA Pro | 1973:8000 | **HDD** mode | slider |
| HORI NSW-230 (Mega39's) | 0F0D:00FB | **Arcade** mode | – (lights itself) |
| HORI PS4-161 (FT DX) | 0F0D:013C | **Arcade** mode | – (lights itself) |

One controller at a time. The serial log says so if a ZhouSensor is in the wrong mode.

## Hardware

- **Any ESP32-S3 DevKitC-style board** (N8R2, N16R8, …). The controller goes on the **USB/OTG** port and settings go over the **UART/COM** port.
  - Most of these boards don't put 5 V on the OTG port, so use a **powered USB hub** between the board and the controller. The firmware supports hubs (it needs Arduino-ESP32 3.x).
  - Some hubs won't come up unless their upstream port sees 5 V too. If yours stays dark, check whether your board has a solder jumper that feeds 5 V to the OTG port (often labelled `USB-OTG`).
- **Espressif ESP32-S3-USB-OTG**: has a real USB-A host port with power, a battery connector and a charger. Build the `esp32s3_usb_otg` env. Choose where host-port power comes from with `set otg_power usb|battery`.

## Setup page (install, settings, console)

Open **`DIVA_Bridge_ESP32_Setup.html`** in Chrome or Edge. It's a single file from the release, or build it yourself (see below). The page header shows the firmware version built into the page. Once connected, it also shows the version on the board and offers an update when the board's is older.

1. Pick the board.
2. Fill in Wi-Fi and the other options on the **Settings** tab.
3. On **Install**, click **Install firmware + settings** and pick the board's port. The page uses that one port for everything that follows.

After writing, the page restarts the board and watches its log. It then shows either the IP with a link to the board's own page, or why Wi-Fi failed (`wrong password`, `network not found`, …).

- **Install:** *Install firmware + settings*, or *Update firmware, keep settings*. Tick *Erase the whole flash first* to clear everything, pairings included.
- **Settings:** *Read from board* and *Write settings*. Writing takes about a second.
- **Console:** status of the running firmware, the **USB device tree** (hubs, ports, what each device is), the live log, and the serial commands (`help`).

The page switches the port between flashing mode (Install and Settings) and the running firmware's console as needed. Disconnecting always leaves the board running its firmware.

The settings live in a 4 KB `divacfg` partition at `0x310000` (see `partitions.csv` and `src/ConfigBlob.h`). The firmware applies a newly written set once at boot. It copies any change made over serial or the web page back into that partition, together with its version, so *Read from board* always shows what's in use. Firmware updates write the bootloader, partition table, `boot_app0` and app separately, so they leave settings and pairings alone.

For boards with a single USB-C port, which is a USB host in this firmware: hold **BOOT** while plugging the board into the PC, and Install and Settings work the same. The console can't follow it there, because after a restart the port belongs to the controller. Use the board's web page instead.

## Build

PlatformIO, using the community [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino-ESP32 3.x):

```bash
pio run -e esp32s3_devkit -t upload      # or -e esp32s3_usb_otg
pio device monitor
```

Flash through the UART/COM port. If the board won't take the upload, hold BOOT while plugging it in.

To build the standalone setup page with both boards' images embedded:

```bash
pio run -e esp32s3_devkit -e esp32s3_usb_otg
python3 tools/build_setup_page.py         # -> dist/DIVA_Bridge_ESP32_Setup.html
```

The version lives in `src/Version.h`. Bump it there; the setup page and the firmware both pick it up.

## On-device web page

Once the board is on Wi-Fi, open **`http://diva-bridge.local/`**, or **`http://<its IP>/`**. The setup page shows the address after it writes. The page has three tabs:

- **Tester:** the controller, drawn like the DIVA Bridge GUI, with the live slider, buttons, D-pad and shoulder buttons. It also shows the report rate, raw packets, and whether a game or the Xbox pad is active. An LED test lights the slider where touched, as a rainbow or in white, and also drives the Divaller's button LEDs.
- **Status:** the exact `DivallerBridge.ini` and BridgeSlider lines for this board, the **USB device tree**, the firmware version, and the Wi-Fi, Bluetooth and client counts.
- **Settings:** the same options as the setup page. Saving restarts the board.

The page only reads the controller, so it doesn't count as a game and the Xbox pad stays on. It runs on port 80, and the bridge port can't be set to 80. The page source is `web/index.html`; run `python3 tools/gen_webpage.py` after editing it.

## Serial console

The setup page's Console tab covers this. Any serial terminal at 115200 baud also works:

```
set ssid My Network
set pass hunter2
save
status
```

`help` lists everything: radio mode, Bluetooth name, slider mapping on the pad, reverse flags, LED forwarding, Xbox profile, pairing reset.

## Using it

### Bluetooth Xbox controller (any PC)

Pair **"DIVA Bridge"** in the system's Bluetooth settings. It shows up as an Xbox controller (XInput on Windows).

| Controller | Xbox |
|---|---|
| △ □ ✕ ○ | Y X A B |
| FN / + / OPTIONS | Start |
| L1 / − / SHARE | View |
| L3 / HOME / PS | Guide |
| L2 / CAPTURE / TOUCHPAD | Share |
| D-pad, L/R, ZL/ZR, stick clicks (HORI, ZhouSensor) | D-pad, LB/RB, LT/RT, LS/RS |
| Slider: left half / right half | left stick X / right stick X |

There are three slider modes on the pad (`set slider …`):
- `motion` (default): a swipe pushes that half's stick in the swipe's direction.
- `position`: the touch position is the stick position.
- `off`.

### PD-Loader (TLAC) with DivallerBridge.dva, PC or the Quest recomp, over Wi-Fi

In `DivallerBridge.ini`, point the plugin at the board instead of at a PC running the bridge:

```ini
[bridge]
host=192.168.1.42        ; the board's address (see "status"), or diva-bridge.local
port=45710
```

No bridge program is needed on the PC. The plugin and its recomp talk to the board directly, as they did to the Steam Deck.

### Mega Mix+ with BridgeSlider, over Wi-Fi

Set BridgeSlider's bridge host/port to the board, the same way.

### Over Bluetooth instead of Wi-Fi (PC)

Run the bridge with the board as its controller. It connects over Bluetooth, does the handshake, and serves the usual `127.0.0.1:45710` to the game, with the GUI and TUI working as usual:

```bash
pip install bleak
python3 diva_bridge.py --device ble --tui         # or pick "DIVA Bridge ESP32 (Bluetooth)" in the GUI
```

Pair the board with the PC first. The bridge connects alongside the system's gamepad driver.

## Latency notes

- Wi-Fi and Bluetooth share one radio. With both on, ESP-IDF requires Wi-Fi modem sleep, which adds jitter to Wi-Fi round trips. If you only use Wi-Fi native mode, `set radio wifi` keeps Wi-Fi fully awake. That also turns off the Xbox pad.
- Bluetooth asks for a 7.5 ms connection interval. The host decides what it actually gets; the log and the board's Status tab show the interval granted.
- **No lost taps.** Each reader has its own *press latch*: a Wi-Fi client between two polls, or the Bluetooth sender between two notifications. A press that starts and ends in between still shows up once. A notification that can't be sent is retried with its presses kept. Over Bluetooth, state also arrives in bursts once per connection event, so a tap's press and release can land microseconds apart. `diva_bridge.py --device ble` therefore keeps every press visible for at least 20 ms (`--ble-min-hold`), so the game, which reads the state once a frame, always sees it.
- **Measuring it:** the ESP32 counts every press, and the count travels with each Bluetooth update. Every 30 s, `diva_bridge.py` logs `BLE link: N updates/s, longest gap X ms; presses A of B (lost L); short taps held H`. `lost` above 0 means presses never reached the PC. `short taps held` counts taps that would have been missed without the minimum hold.
- If the board can't join Wi-Fi, the serial log says why (`network not found`, `wrong password`, …), and the setup page shows that after a write.
- Every USB attach and detach is logged (`[USB] attached: …`, hubs included), so a hub that never shows up is easy to spot.
- A Wi-Fi client that sends nothing for 5 s is dropped, so a sleeping PC doesn't keep the Xbox pad muted. A Bluetooth client stops being native when it unsubscribes from INPUT or disconnects.

## Protocol reference

**Wi-Fi (TCP, default port 45710)** uses the same protocol as `diva_bridge.py`:

| Request | Reply |
|---|---|
| `P` | status(1): controller present |
| `R` ep max | status len data — latest 24-byte Divaller packet |
| `X` ep max | status len data — packet + 2 bytes extended buttons (LE) |
| `W` ep len data | status — LED packet (100-byte `DLa`) |

**Bluetooth LE** uses service `5e1a0000-d1ba-4b1d-9e00-000000d1ba00`, alongside the Xbox HID service:

| UUID suffix | Props | Contents |
|---|---|---|
| `…0001` CONTROL | read, write, notify | write `DIVA`+version → native; `BYE` → leave. Read/notify: `[ver, present, kind, native_clients, VID LE, PID LE]` |
| `…0002` INPUT | read, notify | 24-byte packet + 2 bytes extended buttons (same bytes as an `X` reply) |
| `…0003` LEDS | write, write w/o response | 100-byte `DLa` packet (MTU ≥ 103, or a long write) |

`kind`: 1 Divaller, 2 ZhouSensor, 3 HORI NSW-230, 4 HORI PS4-161.

## Tests (on a PC)

```bash
cd test
g++ -std=c++17 -O1 -o protocol_cli protocol_cli.cpp ../src/DivaProtocol.cpp
python3 test_protocol.py /path/to/DIVA_Bridge_project/diva_bridge.py   # translators vs. diva_bridge.py
g++ -std=c++17 -pthread -Ihost_shim -o net_server net_server.cpp \
    ../src/NetProtocol.cpp ../src/DivaState.cpp ../src/DivaProtocol.cpp
python3 test_net.py /path/to/DIVA_Bridge_project/diva_bridge.py        # TCP protocol, as the plugins use it
g++ -std=c++17 -o config_cli config_cli.cpp ../src/ConfigBlob.cpp
python3 test_setup_page.py ../dist/DIVA_Bridge_ESP32_Setup.html         # setup page vs. a simulated board (Playwright)
python3 test_webui.py ../web/index.html /tmp/shots                     # on-device page vs. a mock API (Playwright)
```

## Source layout

| File | What |
|---|---|
| `src/DivaProtocol.*` | controller → Divaller packets, LED translation (portable C++) |
| `src/ControllerHost.*` | USB host: finds the controller (hub OK), reads it, sends LEDs |
| `src/DivaState.*` | shared latest input / pending LEDs / native-client count |
| `src/NetBridge.*`, `src/NetProtocol.*` | Wi-Fi, mDNS, TCP bridge server |
| `src/BleBridge.*` | Xbox BLE pad + DIVA Bridge GATT service |
| `src/WebUI.*`, `web/index.html` | port-80 tester / status / settings page (packed into `src/WebPage.h`) |
| `src/Settings.*` | NVS settings, the serial console, the settings partition |
| `src/ConfigBlob.*` | settings-partition format (shared with the setup page) |
| `src/Version.h` | firmware version |
| `partitions.csv` | huge_app layout + `divacfg` |
| `tools/setup_page/`, `tools/build_setup_page.py` | setup page template (install / settings / console) and the script that embeds the firmware |
| `lib/ESP32-BLE-CompositeHID` | [Mystfit's library](https://github.com/Mystfit/ESP32-BLE-CompositeHID) (MIT) @ 06d93ea, patched so one central disconnecting doesn't mute the pad for the others |
