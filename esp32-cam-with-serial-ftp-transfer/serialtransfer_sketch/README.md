# ESP32-CAM Serial File Transfer (SerialTransfer Edition)

Serial file transfer between an ESP32-CAM and a Python host, using [PowerBroker2's **SerialTransfer**](https://github.com/PowerBroker2/SerialTransfer) library for packetized, CRC-checked communication.

Replaces the hand-rolled checksum/ACK protocol from the reference implementation with COBS framing + CRC16 handled transparently by the library.

---

## Hardware

- **AI-Thinker ESP32-CAM** (or pin-compatible clone)
- **USB-to-Serial adapter** (e.g., FT232, CP2102, CH340) wired to UART0
- **microSD card** (FAT32 formatted)
- Optional: charging-detect circuit, mode button, LiPo battery

### Wiring (USB ↔ ESP32-CAM)

| USB-Serial | ESP32-CAM |
|-----------|-----------|
| TX        | U0RXD (GPIO3) |
| RX        | U0TXD (GPIO1) |
| GND       | GND           |

> **Do not** connect USB 5V to the ESP32-CAM 5V pin while the CAM is also powered by its own regulator — share GND only, or use a single power source.

---

## Arduino

### Dependencies

Install via **Arduino IDE → Sketch → Include Library → Manage Libraries**:

| Library | Version | Install |
|---------|---------|---------|
| **SerialTransfer** | ≥ 3.1.1 | Search "SerialTransfer" by PowerBroker2 |

Also requires the **ESP32 Arduino core** (≥ 2.0.0):
- Arduino IDE → Preferences → Additional Board Manager URLs:
  ```
  https://dl.espressif.com/dl/package_esp32_index.json
  ```
- Boards Manager → search "esp32" → install

### Upload

1. Connect GPIO0 to GND (enter download mode)
2. Select board: **ESP32 Dev Module** or **AI Thinker ESP32-CAM**
3. Upload speed: **921600**
4. Flash the sketch: `esp32cam_serial_transfer/esp32cam_serial_transfer.ino`
5. Disconnect GPIO0 from GND, press reset

---

## Python Client

### Install

```bash
cd client
pip install -r ../requirements.txt
```

### Usage

```bash
# Interactive REPL
python esp32cam_client.py /dev/ttyUSB0 repl

# List SD card root
python esp32cam_client.py /dev/ttyUSB0 list /sdcard

# Upload a file
python esp32cam_client.py /dev/ttyUSB0 upload photo.jpg /picture99.jpg

# Download a file
python esp32cam_client.py /dev/ttyUSB0 download /picture1.jpg ./saved.jpg

# Delete a file
python esp32cam_client.py /dev/ttyUSB0 delete /picture1.jpg

# Switch to photo mode (device deep-sleeps)
python esp32cam_client.py /dev/ttyUSB0 mode photo

# Verbose debug output
python esp32cam_client.py -v /dev/ttyUSB0 list
```

> On Windows use `COM3` (or similar) instead of `/dev/ttyUSB0`.

---

## Protocol

All packets are COBS-framed and CRC16-validated by the SerialTransfer library.

| Byte 0 | Meaning | Payload |
|--------|---------|---------|
| `0x00` | `CMD_PING` | — |
| `0x10` | `CMD_LIST` | path string |
| `0x11` | `CMD_READ` | path string |
| `0x12` | `CMD_STORE` | 124-byte path + 4-byte size (little-endian) |
| `0x13` | `CMD_DELETE` | path string |
| `0x14` | `CMD_MODE` | 1 byte: `0x00` = photo, `0x01` = transfer |
| `0x01` | `RESP_PONG` | — |
| `0x20` | `RESP_ENTRY` | 1 byte isDir + 4 bytes size + name |
| `0x30` | `RESP_OK` | optional message |
| `0x31` | `RESP_ERROR` | message |
| `0x40` | `DATA_CHUNK` | raw file bytes |
| `0x41` | `END_OF_DATA` | — |

---

## Modes

### Photo Mode (default)

- Captures a JPEG frame
- Saves to SD as `/pictureN.jpg`
- Deep sleeps for the configured interval (default 60s)
- Wakes up via timer or GPIO 15 (button) LOW

### Transfer Mode

- Serial command loop at **921 600 baud** (configurable, see below)
- Commands: `LIST`, `READ`, `STORE`, `DELETE`, `MODE`
- Auto-negotiated by the Python client (two-phase connection)

### Changing the Baud Rate

Both sides must agree. Edit these two lines to the same value:

**Arduino** (`esp32cam_serial_transfer.ino`):
```cpp
#define TRANSFER_BAUD  921600
```

**Python** (`client/esp32cam_client.py`):
```python
TRANSFER_BAUD = 921600
```

Suggested values (fastest → most reliable): `921600`, `460800`, `115200`.

### Switching Modes

**At boot** (within 2 seconds):

```
Send plain text: MODE TRANSFER
```

The Python client handles this automatically as a fallback.

**From transfer mode**:

```bash
python esp32cam_client.py /dev/ttyUSB0 mode photo
```

The device will deep-sleep and resume photo mode on next wake.

---

## File Layout

```
serialtransfer_sketch/
├── README.md
├── requirements.txt
├── esp32cam_serial_transfer/
│   └── esp32cam_serial_transfer.ino
└── client/
    └── esp32cam_client.py
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `ConnectionError` | Try a shorter USB cable or lower `TRANSFER_BAUD` to 460800 or 115200 |
| SD mount fails | Check card is FAT32, ≤32 GB, properly inserted |
| Garbled serial output | Ensure both sides use the same baud rate |
| Slow file transfer | Normal — 247-byte chunks ≈ ~200 KB/s at 921600 baud |
| Corrupt downloaded file / hang mid-transfer | CRC failure caused a dropped packet. Lower baud rate or add per-chunk ACKs (see below) |

---

## Why Data Loss Happens (and Why CRC Doesn't "Fix" It)

SerialTransfer **does** validate every packet with CRC16. When CRC fails, the library silently discards the packet (`available()` returns false). This is correct behavior — a bad packet must not be acted on.

The problem is that **the file-transfer protocol has no retry**. We stream `DATA_CHUNK` packets back-to-back without waiting for an ACK per chunk. If one chunk is dropped:

- **Download (READ):** the Python client accumulates fewer bytes than expected and hangs waiting for `END_OF_DATA`.
- **Upload (STORE):** the ESP32 writes fewer bytes than expected and eventually times out.

### Immediate Fixes

1. **Lower the baud rate** — signal integrity improves dramatically. Default is now `921600`.
2. **Use a short USB cable** — < 30 cm if possible.
3. **Try a different USB-serial adapter** — FT232 is more reliable than CH340 at high speeds.

### Proper Fix (Not Yet Implemented)

Add a per-chunk ACK to the protocol:

```
Sender:   DATA_CHUNK →
Receiver:                    ← RESP_OK
Sender:   DATA_CHUNK →
Receiver:                    ← RESP_OK
... repeat ...
Sender:   END_OF_DATA →
```

This ensures every chunk is acknowledged before the next is sent. Dropped packets simply time out and are resent.

---

## License

MIT — same as the reference implementation and SerialTransfer library.
