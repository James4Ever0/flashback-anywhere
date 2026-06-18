# Plan: ESP32-CAM Serial File Transfer with XMODEM

## Overview

Replace the custom ad-hoc checksum-chunk protocol with **XMODEM-CRC**, a 40+ year standard for serial file transfer. Keep the same command set (`LIST`, `READ`, `STORE`, `DELETE`, `MODE`) and two-phase connection logic.

## Architecture

```
┌──────────────┐      serial (2M baud)      ┌──────────────────┐
│  Python CLI  │  ←──────────────────────→  │  ESP32-CAM       │
│  (xmodem)    │   XMODEM-CRC for payload   │  (Arduino sketch)│
└──────────────┘                            └──────────────────┘
```

Command channel stays text-based (`LIST /sdcard`, `READ /sdcard/foo.jpg`). File payload goes through XMODEM-CRC frames.

---

## 1. Protocol Design

### Why XMODEM-CRC (not original checksum)

| Variant | Block Size | Error Detection | Notes |
|---------|-----------|-----------------|-------|
| XMODEM (original) | 128 B | 1-byte checksum | Weak, obsolete |
| **XMODEM-CRC** | 128 B | 16-bit CRC | **Standard, robust** |
| XMODEM-1K | 1024 B | 16-bit CRC | Faster, still compatible |

We will use **XMODEM-CRC with 1K blocks** (STX/1024-byte blocks) for speed at 2 Mbaud, falling back to SOH/128-byte if the receiver requests it.

### XMODEM Frame Format (1K / CRC)

```
+--------+----------+----------+----------+-----+-----+
|  STX   |  seq#    | ~seq#    | 1024 B   | CRC | CRC |
| 0x02   |  0x01    |  0xFE    | data...  | hi  | lo  |
+--------+----------+----------+----------+-----+-----+
```

- Sender waits for receiver to transmit `'C'` (0x43) = "I want CRC mode"
- Each block: STX, seq, ~seq, 1024 bytes data, 16-bit CRC-CCITT
- Receiver ACKs (0x06) each good block, NAKs (0x15) bad ones
- Sender sends EOT (0x04) when done; receiver ACKs EOT

### Hybrid Protocol: Text Commands + XMODEM Payload

```
Python:  "READ /sdcard/picture1.jpg\n"
ESP32:   "XMODEM 12345\n"   (file size, then switches to XMODEM sender)
Python:  sends 'C' to start XMODEM receive
ESP32:   sends XMODEM blocks...
Python:  ACKs blocks, receives EOT, sends final ACK
ESP32:   returns to text prompt: "> "
```

Same flow reversed for `STORE`:

```
Python:  "STORE /sdcard/new.jpg 67890\n"
ESP32:   "XMODEM READY\n"   (switches to XMODEM receiver)
Python:  sends 'C' to initiate, then XMODEM blocks...
ESP32:   ACKs/NACKs blocks, receives EOT
ESP32:   "> "
```

---

## 2. Arduino Sketch (`xmodem_sketch.ino`)

### Libraries

| Library | Source | Version | Purpose |
|---------|--------|---------|---------|
| *(built-in)* | ESP32 core | ≥ 2.0.14 | `Serial`, `FS`, `SD_MMC`, `esp_camera.h` |
| *(embedded)* | This sketch | — | Lightweight XMODEM encoder/decoder (no external dep) |

> **Why no Arduino XMODEM library?** The available libraries (`XModem` by dookiedev, `Xmodem` by various authors) are either receiver-only, unmaintained, or have incompatible APIs. XMODEM is ~150 lines to implement correctly; embedding it avoids dependency fragility.

### Files

```
xmodem_sketch/
└── xmodem_sketch.ino          # Single-file sketch
```

### Key Implementation Details

- **CRC-16-CCITT**: polynomial `0x1021`, init `0x0000`. Standard XMODEM CRC.
- **1K blocks (STX)**: primary. Falls back to 128-byte (SOH) if Python client signals via shorter `CAN`/`NAK` dance.
- **Sender** (`readFile` path): reads SD file, pads last block with `0x1A` (CPM EOF), streams STX frames.
- **Receiver** (`storeFile` path): writes incoming blocks to SD temp file, renames on success.
- **Timeout**: 10 s per block (generous for SD write latency).
- **Retries**: 10 attempts per block before aborting.

### Commands (same as reference)

| Command | Response | Notes |
|---------|----------|-------|
| `LIST <path>` | Text listing | No change |
| `READ <path>` | `XMODEM <size>\n` → XMODEM stream | File → PC |
| `STORE <path> <size>` | `XMODEM READY\n` → XMODEM receive | PC → file |
| `DELETE <path>` | `[OK]` or `[ERROR]` | No change |
| `MODE PHOTO` | `[OK]` → deep sleep | No change |
| `HELP` | Command list | No change |

---

## 3. Python Client (`client.py` + `xmodem_transfer.py`)

### Libraries

| Package | PyPI | Version | License | Purpose |
|---------|------|---------|---------|---------|
| `pyserial` | [pypi](https://pypi.org/project/pyserial/) | ≥ 3.5 | BSD | Serial port I/O |
| `xmodem` | [pypi](https://pypi.org/project/xmodem/) | ≥ 0.4.7 | MIT | XMODEM protocol engine |

```bash
pip install "pyserial>=3.5" "xmodem>=0.4.7"
```

> **`xmodem`** package repo: https://github.com/tehmaze/xmodem  
> Docs: https://python-xmodem.readthedocs.io/

### Files

```
xmodem_sketch/
├── client.py                  # argparse CLI (repl, list, upload, download, delete)
├── xmodem_transfer.py         # ESP32Serial class + XMODEM bridge
└── requirements.txt           # pinned dependencies
```

### Key Implementation Details

- **`xmodem.XMODEM(getc, putc)`**: instantiate with custom `getc`/`putc` that read/write the `pyserial.Serial` object.
- **Two-phase connection**: identical to reference — try 2M baud first, fall back to 115200 + `MODE TRANSFER`.
- **`read_file(path)`**: send `READ <path>`, parse `XMODEM <size>`, call `xmodem.recv(stream)`.
- **`store_file(path, data)`**: send `STORE <path> <size>`, parse `XMODEM READY`, call `xmodem.send(stream)`.
- **Temp stream**: use `io.BytesIO` for in-memory buffer; for large files, stream directly from disk.

---

## 4. File Layout (this repo)

```
xmodem_sketch/
├── plan.md                    # this file
├── xmodem_sketch.ino          # Arduino sketch
├── client.py                  # Python CLI entry point
├── xmodem_transfer.py         # Python serial + XMODEM library
├── requirements.txt           # pip dependencies
└── README.md                  # Usage, wiring, library install
```

---

## 5. Implementation Order

1. **Arduino XMODEM core** — CRC-16, send_block, recv_block, EOT handling
2. **Arduino command integration** — wire `READ`/`STORE` to XMODEM paths
3. **Python `xmodem_transfer.py`** — `ESP32Serial` class with `xmodem` package
4. **Python `client.py`** — argparse CLI with `repl`, `list`, `upload`, `download`, `delete`
5. **Integration test** — loopback: upload → list → download → compare checksums

---

## 6. Risk & Fallback

| Risk | Mitigation |
|------|-----------|
| XMODEM 1K too large for ESP32 serial buffer | Use 128-byte (SOH) blocks; `xmodem` lib supports both |
| `xmodem` PyPI package API changes | Pin to `>=0.4.7,<1.0` in `requirements.txt` |
| CRC mismatch across implementations | Verify with known test vector before integration |
| SD write latency > XMODEM timeout | Set block timeout to 10 s; `xmodem` default is 60 s |

