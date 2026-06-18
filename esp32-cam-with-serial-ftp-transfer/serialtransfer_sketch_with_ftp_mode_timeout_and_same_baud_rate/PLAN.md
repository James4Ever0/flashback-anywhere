# Plan: ESP32-CAM Serial File Transfer

## Objective

Implement an Arduino sketch + Python client for ESP32-CAM ↔ PC serial file transfer, using the **SerialTransfer** library (COBS framing + CRC16) instead of the reference's custom checksum protocol.

---

## 1. Arduino Libraries

| Library | Version | Source | Purpose |
|---------|---------|--------|---------|
| **SerialTransfer** | ≥3.1.1 | Arduino Library Manager / [GitHub](https://github.com/PowerBroker2/SerialTransfer) | Packetized serial communication with COBS framing + CRC16 |
| **pySerialTransfer** | ≥2.6.9 | `pip install pySerialTransfer` | Python counterpart |
| **pyserial** | ≥3.5 | `pip install pyserial` | Underlying serial port access |

> The SerialTransfer library (`txObj`/`rxObj` API) replaces the hand-rolled djb2+ACK/NACK protocol from the reference. It handles framing, checksums, and retransmission internally.

---

## 2. Arduino Sketch Structure

### File: `esp32cam_serial_transfer/esp32cam_serial_transfer.ino`

**Architecture:** Same dual-mode design as reference — photo mode and transfer mode.

**Modes:**
- `MODE_PHOTO` (default): Capture → save to SD → deep sleep → repeat
- `MODE_TRANSFER`: Listen on serial for file commands

**SerialTransfer integration:**
- Use `SerialTransfer` object on `Serial` (or `Serial1` if needed)
- Define a packet structure for commands + file data
- Send/receive binary payloads via `myTransfer.sendData()` / `myTransfer.available()`

**Command protocol (packet-based instead of text):**

| Packet Type | Payload | Direction | Description |
|-------------|---------|-----------|-------------|
| `CMD_LIST`   | `char path[128]` | PC → ESP32 | List directory |
| `CMD_READ`   | `char path[128]` | PC → ESP32 | Download file |
| `CMD_STORE`  | `char path[128] + uint32_t size` | PC → ESP32 | Init upload |
| `CMD_DELETE` | `char path[128]` | PC → ESP32 | Delete file |
| `CMD_MODE`   | `uint8_t mode` | PC → ESP32 | Switch mode |
| `RESP_OK`    | varies | ESP32 → PC | Success response |
| `RESP_ERROR` | `char msg[128]` | ESP32 → PC | Error response |
| `DATA_CHUNK` | `uint8_t data[N]` | bidirectional | File data chunks |

**Key differences from reference:**
- No manual `CHECKSUM:` / `ACKCHECKSUM` / `ACKDATA` dance — SerialTransfer handles it
- No custom chunk framing — packets are self-delimiting via COBS
- File data sent as multiple `DATA_CHUNK` packets, each CRC-validated by the library

---

## 3. Python Client Structure

### File: `client/esp32cam_client.py`

A single library file (can split TUI later) providing:

```python
class ESP32CamClient:
    def connect(self) -> bool
    def disconnect(self) -> None
    def list_files(self, path: str) -> list[dict]
    def read_file(self, path: str) -> bytes
    def store_file(self, path: str, data: bytes) -> bool
    def delete_file(self, path: str) -> bool
    def switch_mode(self, mode: str) -> bool
```

**Connection logic (same two-phase as reference):**
1. Try 2,000,000 baud — send probe packet, look for response
2. Fall back to 115,200 — wait for boot text, send `MODE TRANSFER`, reconnect at 2M

**SerialTransfer Python API:**
- `from pySerialTransfer import pySerialTransfer as txfer`
- `link = txfer.SerialTransfer(port, baud)`
- `link.txObj(data, start_pos)` / `link.rxObj(type, start_pos, byte_format)`
- `link.send(len)` / `link.available()` / `link.rxLen`

---

## 4. File Layout

```
serialtransfer_sketch/
├── PLAN.md                          # This file
├── README.md                        # Usage, wiring, dependencies
├── requirements.txt                 # Python deps: pyserial, pySerialTransfer
├── esp32cam_serial_transfer/        # Arduino sketch folder
│   └── esp32cam_serial_transfer.ino
└── client/                          # Python client
    └── esp32cam_client.py
```

---

## 5. Implementation Steps

| Step | Task | Files |
|------|------|-------|
| 5.1 | Port reference Arduino sketch, swap custom protocol for SerialTransfer packets | `.ino` |
| 5.2 | Port reference Python library, swap custom protocol for pySerialTransfer | `client.py` |
| 5.3 | Write `README.md` with wiring diagram, library install steps, and usage | `README.md` |
| 5.4 | Write `requirements.txt` with pinned versions | `requirements.txt` |
| 5.5 | Test compile Arduino sketch (verify no syntax errors) | — |
| 5.6 | Smoke-test Python imports | — |

---

## 6. Open Questions / Decisions

1. **SerialTransfer buffer size:** Default is 254 bytes. For file chunks we may need to increase `MAX_PACKET_SIZE` in the library config or send many small packets. The reference used 2KB chunks — we should benchmark throughput with the default 254-byte payload.
2. **Baud rate:** Keep 2,000,000 baud as in reference, or test stability at lower rates. ESP32-CAM + USB-Serial at 2M is usually stable with short cables.
3. **SD card pins:** Use the same AI-Thinker pin mapping as reference (no change needed).
4. **Deep sleep wakeup:** Keep GPIO 15 ext0 wakeup and timer wakeup as in reference.

---

## 7. Success Criteria

- [ ] Arduino sketch compiles for ESP32 board target
- [ ] Python client imports without errors (`python -c "import esp32cam_client"`)
- [ ] Protocol matches: LIST, READ, STORE, DELETE, MODE all functional
- [ ] File transfer is reliable (CRC-checked by SerialTransfer, no manual ACK dance)
- [ ] Both connection phases work (direct 2M, and 115200→2M fallback)
- [ ] Photo mode + deep sleep cycle preserved from reference
