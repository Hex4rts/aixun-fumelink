# aixun-fumelink

Make **any fume extractor** follow an AiXun T420D soldering station's *FumeSuck Link*,
the feature that turns the extractor on when you pick up an iron and off when you put it back.
AiXun only supports its own extractors (ES02/ES03). FumeLink is an ESP32 that joins the
station's Bluetooth link the way an ES02 does, and switches your extractor through a
transistor (or relay).

```
T420D  ──BLE──►  ESP32-S3-Zero (FumeLink)  ──GPIO4──►  NPN  ──►  extractor speed input
"iron working"       poses as an ES02            fan on/off
```

> Not affiliated with or endorsed by AiXun / JCID. "AiXun", "T420D", "ES02" and "ES03"
> are their trademarks and are used here only to describe compatibility.

## Status

| Part | State |
|---|---|
| BLE link to a T420D (connect, handshake, live iron state) | working, tested |
| Reconnect after power-cycling the ESP32 or the station | working (ESP ~3 s, station ~6 s after boot) |
| Fan on when a handle leaves the stand, run-on timer, then off | working (LED + GPIO) |
| BOOT button manual toggle, with or without the station | working |
| WiFi hotspot + web page (status, run-on, fan on/off, password) | working |
| Firmware update over WiFi (OTA) | implemented, **not yet tested** |
| Transistor stage: GPIO and base drive | verified on the bench |
| Transistor stage: collector switching, real extractor | **not yet verified** |

Tested with one T420D whose BLE module reports firmware V1.05. Other AiXun stations that
advertise an ES02-capable FumeSuck Link probably work too, but haven't been tried.

## Hardware

- **ESP32-S3-Zero** (Waveshare). Any ESP32 with BLE works; change `LED_PIN` / `BUTTON_PIN`.
- **One NPN transistor**, small-signal: C945, 2N2222, 2N3904, S8050, BC547…
- **Resistors:** 1 kΩ, 4.7 kΩ, 10 kΩ.

### Wiring: extractor with a 0–5 V speed pot

Many extractors set the fan speed with a potentiometer whose wiper (here the yellow wire)
gives the controller 0–5 V. FumeLink leaves the pot alone and pulls the controller's input
to ~0 V when the fan should be off. You set the speed with the pot as usual.

Cut the wiper wire and insert the 1 kΩ:

```
pot wiper (pot side) ──[ 1 kΩ ]──┬──── wiper wire, controller side
                                 │
                                 C
ESP GPIO4 ──[ 4.7 kΩ ]──┬─────── B      NPN
                       [ 10 kΩ ]        E
                        │               │
ESP GND ────────────────┴───────────────┴──── controller GND (black)
```

- **GPIO4 HIGH** → transistor on → controller input ≈ 0.1 V → **fan off**.
- **GPIO4 LOW** → transistor off → controller sees the pot → **fan at your set speed**.
- The **1 kΩ protects the pot.** Without it, the transistor would short the wiper to ground
  and could burn the pot track at high settings.
- The **10 kΩ** keeps the transistor off while the ESP boots or is unpowered, so if the
  ESP dies the fan simply runs at pot speed.
- **The ESP's GND must be connected to the controller's GND.**
- First check that your fan actually **stops** with the pot at minimum (0 V). If it still
  spins at 0 V, this method can't turn it off. Use a relay on the motor supply instead and
  set `RELAY_ACTIVE_HIGH` to match your relay module.
- Avoid GPIO 0, 3, 45, 46 (strapping), 19/20 (USB) and 21 (the S3-Zero's LED).
- Powering the ESP from the controller: the S3-Zero draws around 100 mA with BLE active.
  Check the controller's 5 V output can supply that before relying on it.

⚠️ If you switch mains instead, use a properly rated relay module or SSR, and keep mains
wiring enclosed and away from the low-voltage side.

### Pinout tip (C945)

The C945 is usually **E-C-B** (flat face toward you, legs down), unlike the 2N3904's E-B-C.
Check with a multimeter in diode mode: with the red probe on the base, you read about
0.6–0.7 V to both other legs, and nothing in any other combination.

## Using it

**LED:**

| Colour | Meaning |
|---|---|
| Blue | searching for the station |
| Green | linked, iron idle, fan off |
| Red | iron working, **fan on** |
| Orange | iron back in its stand, run-on timer counting down |

**BOOT button:** toggles the fan by hand, with or without the station. The station takes
control again at its next change (handle lifted or replaced).

**On the station:** enable *FumeSuck Link* in its system settings.

### WiFi page (no need to open the enclosure)

FumeLink runs its own WiFi hotspot. It never joins your home network.

1. Connect a phone or PC to WiFi **`FumeLink`**, password **`fumelink`**.
2. Open **http://192.168.4.1**.
3. **Change the hotspot password straight away.** The default is published here, and anyone
   connected to the hotspot can change settings and upload firmware.

The page shows the fan state (with the run-on countdown), whether the station is linked,
and each channel's state, temperature and setpoint. It also lets you:

- switch the fan **ON / OFF** by hand (holds until the iron next changes state)
- set the **run-on time** in seconds
- change the **hotspot password** (8–63 characters; you'll be disconnected and must
  reconnect with the new one)
- **upload new firmware** (see below)

Settings are stored on the ESP32 (NVS) and survive reboots and power cuts. They're never in
the source code.

### Updating firmware over WiFi

1. Build the `.bin`:
   - Arduino IDE: *Sketch → Export Compiled Binary*, then use `FumeLink.ino.bin` from the `build` folder.
   - `arduino-cli`: add `--output-dir build` to the compile command below and use `build/FumeLink.ino.bin`.
2. On the page, choose the file under *Firmware update* and press **Upload**.
3. The board reboots into the new firmware. If the upload fails or is interrupted, the
   current firmware keeps running (the ESP32 has two firmware slots).

Settings are kept across updates.

### Serial

USB serial (115200 baud) logs every link and state change. Commands:

| Command | Effect |
|---|---|
| `relay on` / `relay off` | switch the fan by hand |
| `runon <ms>` | set and save the run-on time |
| `appass <password>` | set and save the hotspot password (8–63 characters) |

`appass` is the recovery path if you forget the hotspot password.

> Closing the serial port with default settings resets the ESP32-S3 (its built-in USB
> serial treats the DTR/RTS lines dropping as a reset request). That's normal and not a crash.

## Building and flashing

Clone into a folder named `FumeLink` (Arduino requires the folder name to match the `.ino`):

```
git clone https://github.com/Hex4rts/aixun-fumelink.git FumeLink
```

Arduino IDE settings:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Enabled |
| USB Mode | Hardware CDC and JTAG |
| Flash Size | 4MB |
| **Partition Scheme** | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** |

Or with `arduino-cli` (arduino-esp32 core 3.3.x):

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=4M,PartitionScheme=min_spiffs --output-dir build FumeLink
arduino-cli upload  -p COM26 --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=4M,PartitionScheme=min_spiffs --input-dir build
```

**The first flash must be over USB with the Minimal SPIFFS partition scheme.** The firmware
is about 1.2 MB, which nearly fills the default scheme's 1.3 MB firmware slots, and the
partition layout can only be changed over USB. After that, update over WiFi.

Settings at the top of `FumeLink.ino`: `RELAY_PIN`, `RELAY_ACTIVE_HIGH`, `runOnMs` (default
only; the saved value wins), `LED_PIN`, `BUTTON_PIN`, `STATION_PREFIX`, `AP_SSID`,
`AP_DEFAULT_PASS`.

## Testing the transistor stage

Do this on the bench before cutting into the extractor. Measure everything against the ESP's GND.

1. **GPIO4 alone, nothing connected.** It should read ≈ 3.3 V after boot (fan off). Press
   BOOT: ≈ 0 V and the LED turns red. Press again: back to 3.3 V.
2. **Base drive.** Add 4.7 kΩ → base, 10 kΩ base → GND, emitter → GND. The base should read
   ≈ 0.65–0.75 V with the fan off and 0 V with the fan on. If it reads near 3.3 V, the
   resistor isn't reaching the base leg.
3. **Switching.** Collector → 1 kΩ → 5 V. The collector should read ≈ 0.05–0.2 V with the
   fan off and ≈ 5 V with the fan on.
4. **In the extractor.** With the pot at your chosen speed, the fan should run with the LED
   red and stop with the LED green or blue.

## Troubleshooting

- **The station's Bluetooth icon blinks B))) / B forever.** That was the bug described in
  *Why a custom GATT client* below, and it's fixed in this sketch. If you see it with your
  own code, that's the cause.
- **The ESP stays blue.** The station advertises only while nothing is connected to it,
  and it accepts one extractor at a time. Disconnect anything else (phone, PC, a real ES02),
  or power-cycle the station to clear a stuck link.
- **The fan never stops.** Check the shared ground, the transistor pinout, and that your
  extractor really stops at 0 V on its speed input.
- **Forgot the hotspot password.** Connect USB and send `appass <new password>` over serial.
- **The page doesn't load.** Make sure you're connected to the `FumeLink` network (phones
  sometimes drop a WiFi without internet; tell yours to stay connected), then open
  `http://192.168.4.1` with `http`, not `https`.

## How it works: the link protocol

Worked out from the ES02 / ES03 extractor and T420D firmware images AiXun publishes for
updates, then confirmed live against a T420D. No AiXun code is included in this repository.

**Roles.** The station is the GATT **server** and advertises as `T420D_` followed by one
type byte, with 16-bit service **`0x00EE`**:

| Characteristic | Properties | Use |
|---|---|---|
| `EE01` | read | station state, as a TLV list |
| `EE02` | write | requests from the extractor |

The extractor (here, FumeLink) is the BLE **client**. It finds the station, connects,
discovers `0x00EE`, and then:

1. **Hello**, written to `EE02`:
   ```
   '1' <len> "ES02_<12 hex digits>"   '0' 01 01
   ```
   The station accepts names starting with `ES02` (it also knows `BS08` and `LOCK`, which
   are other accessories). On success the `'0'` record in `EE01` changes to `02`
   ("connection successful").
2. **Subscribe**, written to `EE02`: `'_' 01 'B'`. From then on `EE01` includes a `'B'` record.
3. **Poll** `EE01`. It's a list of `[tag][length][value]` records:

| Tag | Content |
|---|---|
| `'0'` (0x30) | link state; `02` = accepted |
| `'9'` (0x39) | `02 <channels>`, then per channel `[flag][temp LE16][setpoint LE16][power %]`; flag `1` = standby, `2` = working |
| `'B'` (0x42) | `04 <mask> 00 <v0…v7>`; **mask bit 6 (`0x40`) = extractor power**: set while a handle is out of its stand |

Observed live: the `'B'` mask is `0x01` with both handles in their stands, and becomes
`0x41` the moment either handle is lifted, at the same time as that channel's flag goes
from 1 to 2 and its temperature climbs to the setpoint.

### Why a custom GATT client

With arduino-esp32 3.3.x on NimBLE, `BLEClient::connect()` starts an MTU exchange as soon
as the link is up. The T420D starts its own MTU exchange first, so the library's call
returns `BLE_HS_EALREADY`, which the library treats as a failed connect. That leaves a
half-open link (the station stops advertising) and retries forever. FumeLink therefore
uses the scanner from the Arduino BLE library but talks to NimBLE directly for the
connection (`ble_gap_connect`, `ble_gattc_disc_svc_by_uuid`, `ble_gattc_write_flat`,
`ble_gattc_read_long`).
