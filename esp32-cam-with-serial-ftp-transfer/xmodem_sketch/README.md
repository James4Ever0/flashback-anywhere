# ESP32-CAM Serial File Transfer (XMODEM-CRC)

Arduino sketch + Python client for file transfer between an ESP32-CAM and a PC
over serial using the **XMODEM-CRC** protocol.

## Hardware

- **AI-Thinker ESP32-CAM** (or compatible OV2640 module)
- SD card (microSD via MMC, 1-bit or 4-bit)
- USB-to-serial adapter (e.g. FT232, CP2102, **CH340**)

### Wiring (programming)

| USB-TTL | ESP32-CAM |
|---------|-----------|
| GND     | GND       |
| TXD     | U0R (GPIO3) |
| RXD     | U0T (GPIO1) |

## Arduino

### Library dependency

**xmodem-lib** by gilman88  
https://github.com/gilman88/xmodem-lib

Install manually:

```bash
# Clone into your Arduino libraries folder
cd ~/Arduino/libraries
git clone https://github.com/gilman88/xmodem-lib.git
```

Or use **Sketch → Include Library → Add .ZIP Library** in the Arduino IDE.

### Board settings

- **Board:** *ESP32 Dev Module* (or AI-Thinker ESP32-CAM)
- **Partition Scheme:** Huge APP (3 MB No OTA / 1 MB SPIFFS) — recommended for camera + SD

### Configurable baud rate

The transfer baud rate is set at **compile time** via `#define`:

```cpp
#ifndef TRANSFER_BAUD
#define TRANSFER_BAUD  921600   // CH340 reliable max
#endif
```

Override at compile time (PlatformIO):

```ini
build_flags = -DTRANSFER_BAUD=115200
```

Or edit the `#define` directly in the sketch.

| Chip     | Reliable max | Notes                          |
|----------|-------------|--------------------------------|
| CH340    | **921600**  | Default — stable on most units |
| FT232RL  | 2000000     | Can go higher                  |
| CP2102   | 921600      | Similar to CH340               |

The **boot baud** is always 115200 for the initial handshake.

### Flashing

1. Connect GPIO0 to GND (bootloader mode)
2. Upload `xmodem_sketch.ino`
3. Disconnect GPIO0 from GND, press RESET

### Boot behavior

At boot the device starts in **photo mode** and waits 3 seconds for a serial
command:

```
MODE TRANSFER
```

If received, it switches to transfer mode at the configured `TRANSFER_BAUD`.
Otherwise it captures a photo, saves to SD, and enters deep sleep.

## Python Client

### Install dependencies

```bash
pip install -r requirements.txt
```

| Package   | Version | License | Source                                |
|-----------|---------|---------|---------------------------------------|
| `pyserial`| ≥ 3.5   | BSD     | https://pypi.org/project/pyserial/    |
| `xmodem`  | ≥ 0.4.7 | MIT     | https://github.com/tehmaze/xmodem     |

### Usage

```bash
# Interactive REPL
python client.py /dev/ttyUSB0 repl

# List SD card root
python client.py /dev/ttyUSB0 list /sdcard

# Upload a file
python client.py /dev/ttyUSB0 upload ./photo.jpg /sdcard/picture99.jpg

# Download a file
python client.py /dev/ttyUSB0 download /sdcard/picture1.jpg ./saved.jpg

# Delete a file
python client.py /dev/ttyUSB0 delete /sdcard/old.jpg

# Switch device back to photo mode (enters deep sleep)
python client.py /dev/ttyUSB0 mode photo

# Use a different baud rate (e.g. 115200 for CP2102 on Linux)
python client.py --baud 115200 /dev/ttyUSB0 list /sdcard

# Verbose debug output
python client.py -v /dev/ttyUSB0 list /sdcard
```

### Connection flow

The client tries the configured **transfer baud** first. If the device is in
photo mode or asleep, it falls back to **115200 baud**, waits for the boot
prompt, sends `MODE TRANSFER`, then reconnects at the transfer baud.

## Protocol

### Text commands (all modes)

| Command | Description |
|---------|-------------|
| `LIST <path>` | List directory entries |
| `DELETE <path>` | Delete a file |
| `MODE PHOTO` | Switch to photo mode (deep sleep) |
| `MODE TRANSFER` | Switch to transfer mode |
| `HELP` | Show command list |

### File transfer commands (transfer mode only)

**READ** — device → host via XMODEM-CRC

```
Host:   READ /sdcard/file.jpg
Device: XMODEM 12345
Host:   [xmodem package sends 'C', receives SOH blocks ... EOT]
Device: [prompt]
```

**STORE** — host → device via XMODEM-CRC

```
Host:   STORE /sdcard/file.jpg 12345
Device: XMODEM READY
Device: [xmodem-lib sends 'C', receives SOH blocks ... EOT]
Device: [prompt]
```

Block size is **128 bytes** (SOH). CRC-16-CCITT (`0x1021` polynomial).

## File layout

```
xmodem_sketch/
├── xmodem_sketch.ino      # Arduino sketch (depends on xmodem-lib)
├── xmodem_transfer.py     # Python serial + XMODEM library
├── client.py              # Python CLI entry point
├── requirements.txt       # pip dependencies
├── xmodem-lib-readme.md   # xmodem-lib documentation
├── plan.md                # Design plan
└── README.md              # This file
```
