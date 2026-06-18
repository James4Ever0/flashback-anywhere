#!/usr/bin/env python3
"""ESP32-CAM Serial File Transfer Client — pySerialTransfer edition.

Handles two-phase connection, packet-based file operations, and an
interactive REPL over a serial link to the SerialTransfer firmware.

NOTE ON DATA LOSS:
    SerialTransfer does CRC16 validation internally. When a packet fails
    CRC, it is silently discarded (available() returns false). The file
    transfer protocol streams DATA_CHUNK packets without per-chunk ACKs.
    If even one chunk is dropped, the file is corrupt or the receiver
    hangs.

    Fixes:
        1. Lower TRANSFER_BAUD below (try 921600, 460800, or 115200).
        2. Use a short, high-quality USB cable.
        3. Add per-chunk ACK to the protocol if drops persist.

Dependencies (see requirements.txt):
    pyserial        >= 3.5
    pySerialTransfer == 2.1.1
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time

# pySerialTransfer uses `serial` underneath.
from pySerialTransfer import pySerialTransfer as txfer

# ---------------------------------------------------------------------------
# Connection constants — must match the Arduino sketch
# ---------------------------------------------------------------------------
# 2000000 is fast but may cause data loss with long/cheap USB cables.
# Try 921600, 460800, or 115200 if you see CRC errors or dropped packets.
# Must match TRANSFER_BAUD in esp32cam_serial_transfer.ino.
TRANSFER_BAUD = 921600

# ---------------------------------------------------------------------------
# Protocol constants — must match the Arduino sketch
# ---------------------------------------------------------------------------
CMD_PING = 0x00
CMD_LIST = 0x10
CMD_READ = 0x11
CMD_STORE = 0x12
CMD_DELETE = 0x13
CMD_MODE = 0x14

RESP_PONG = 0x01
RESP_ENTRY = 0x20
RESP_ERROR = 0x31
RESP_OK = 0x30

DATA_CHUNK = 0x40
END_OF_DATA = 0x41

MODE_PHOTO = 0
MODE_TRANSFER = 1

# Must match PAYLOAD_SIZE in the sketch (248) minus 1 for type byte.
_FILE_CHUNK_SIZE = 247


class ESP32CamError(Exception):
    """Raised when the device responds with an error or protocol mismatch."""


class ESP32CamClient:
    """High-level serial client for the ESP32-CAM SerialTransfer firmware."""

    def __init__(self, port: str, timeout: int = 30, verbose: bool = False):
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        self.link: txfer.SerialTransfer | None = None
        self.baud: int | None = None

    # ------------------------------------------------------------------
    # Logging
    # ------------------------------------------------------------------
    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[lib] {msg}")

    # ------------------------------------------------------------------
    # Connection
    # ------------------------------------------------------------------
    def connect(self) -> bool:
        """Two-phase connection.

        1. Try TRANSFER_BAUD and send a PING probe.
        2. Fall back to 115 200 baud, wait for boot text, send MODE TRANSFER,
           then reconnect at TRANSFER_BAUD.
        """
        self._log(f"connect() called, port={self.port}, timeout={self.timeout}")

        self._log(f"Phase 1: trying {TRANSFER_BAUD} baud...")
        if self._try_baud(TRANSFER_BAUD):
            self._log(f"Connected at {self.baud} baud")
            return True

        self._log("Phase 1 failed. Phase 2: 115 200 baud fallback...")
        if self._try_fallback():
            self._log(f"Connected at {self.baud} baud after fallback")
            return True

        self._log("All connection attempts failed")
        raise ConnectionError(f"Could not connect to {self.port}")

    def disconnect(self) -> None:
        self._log("disconnect() called")
        if self.link and self.link.close:
            self._log("Closing serial port")
            self.link.close()
        self.link = None
        self._log("Disconnected")

    def _try_baud(self, baud: int) -> bool:
        """Open port at *baud*, send PING, look for PONG."""
        self._log(f"_try_baud({baud}): opening...")
        try:
            self.link = txfer.SerialTransfer(self.port, baud)
            self.link.open()
            self._log(f"  Port opened")
            time.sleep(0.2)

            self._log("  Sending PING...")
            self._send_raw(CMD_PING, b"")

            self._log("  Waiting for PONG (5s)...")
            start = time.time()
            while time.time() - start < 5:
                if self.link.available():
                    rx = self.link.rxBuff
                    pkt_type = rx[0]
                    self._log(f"  Received packet type 0x{pkt_type:02x}")
                    if pkt_type == RESP_PONG:
                        self.baud = baud
                        self._log("  PONG received — transfer mode confirmed")
                        return True
                time.sleep(0.05)

            self._log("  No PONG received")
            self.link.close()
            self.link = None
            return False
        except Exception as exc:
            self._log(f"  Exception: {type(exc).__name__}: {exc}")
            if self.link:
                self.link.close()
                self.link = None
            return False

    def _try_fallback(self) -> bool:
        """Wait for boot text at 115200, send MODE TRANSFER, reconnect at 2M."""
        self._log(f"_try_fallback(): opening at 115200, waiting up to {self.timeout}s...")
        try:
            import serial as pyserial_mod

            ser = pyserial_mod.Serial(self.port, 115_200, timeout=1)
            self._log("  Port opened at 115200")
            time.sleep(0.2)
            ser.reset_input_buffer()

            response = b""
            start = time.time()
            last_len = 0
            while time.time() - start < self.timeout:
                if ser.in_waiting:
                    chunk = ser.read(ser.in_waiting)
                    response += chunk
                    if len(response) != last_len:
                        self._log(f"  Received {len(chunk)} bytes (total {len(response)})")
                        last_len = len(response)
                    if b"Send 'MODE TRANSFER' to switch" in response:
                        self._log("  Found boot trigger text")
                        self._log("  Sending MODE TRANSFER...")
                        ser.write(b"MODE TRANSFER\n")
                        ser.flush()
                        time.sleep(1)
                        ser.close()
                        self._log(f"  Port closed, waiting 0.5s before reconnect at {TRANSFER_BAUD}...")
                        time.sleep(0.5)
                        return self._try_baud(TRANSFER_BAUD)
                time.sleep(0.1)

            self._log(f"  Fallback timeout. Preview: {response[:300]!r}")
            ser.close()
            return False
        except Exception as exc:
            self._log(f"  Exception: {type(exc).__name__}: {exc}")
            return False

    # ------------------------------------------------------------------
    # Low-level I/O
    # ------------------------------------------------------------------
    def _send_raw(self, pkt_type: int, payload: bytes) -> None:
        """Write a single packet to the tx buffer and send it."""
        if self.link is None:
            raise ESP32CamError("Not connected")
        self.link.txBuff[0] = pkt_type
        plen = len(payload)
        # pySerialTransfer.txBuff is a bytearray; slice-assign works.
        self.link.txBuff[1 : 1 + plen] = payload
        self.link.send(1 + plen)
        # pySerialTransfer versions vary; .ser / .port / .connection may not exist.
        # Just give the OS a moment to push the packet onto the wire.
        time.sleep(0.003)
        self._log(f"TX type=0x{pkt_type:02X} len={1 + plen}")

    def _recv_packet(self, timeout: float = 10.0) -> tuple[int, bytes]:
        """Block until a packet arrives, then return (type, payload)."""
        if self.link is None:
            raise ESP32CamError("Not connected")
        start = time.time()
        while time.time() - start < timeout:
            if self.link.available():
                rx = self.link.rxBuff
                pkt_type = rx[0]
                payload = bytes(rx[1 : self.link.bytesRead])
                self._log(f"RX type=0x{pkt_type:02X} len={self.link.bytesRead}")
                return pkt_type, payload
            time.sleep(0.005)
        raise ESP32CamError(f"Packet receive timeout ({timeout}s)")

    # ------------------------------------------------------------------
    # Commands
    # ------------------------------------------------------------------
    def list_files(self, path: str = "/sdcard") -> list[dict]:
        """List directory contents."""
        self._log(f"list_files('{path}')")
        self._send_raw(CMD_LIST, path.encode("utf-8"))

        entries: list[dict] = []
        while True:
            pkt_type, payload = self._recv_packet(timeout=10)
            if pkt_type == RESP_ENTRY:
                if len(payload) < 6:
                    continue
                is_dir = payload[0] != 0
                size = struct.unpack("<I", payload[1:5])[0]
                name = payload[5:].decode("utf-8", errors="replace").rstrip("\x00")
                self._log(f"  {'DIR' if is_dir else 'FILE'}: {name} ({size} bytes)")
                entries.append({"type": "dir" if is_dir else "file", "size": size, "name": name})
            elif pkt_type == RESP_OK:
                break
            elif pkt_type == RESP_ERROR:
                msg = payload.decode("utf-8", errors="replace")
                raise ESP32CamError(f"LIST failed: {msg}")
            else:
                self._log(f"  Unexpected packet 0x{pkt_type:02x}")

        self._log(f"  Total entries: {len(entries)}")
        return entries

    def read_file(self, path: str) -> bytes:
        """Download a file from the device — stop-and-wait per chunk."""
        self._log(f"read_file('{path}')")
        self._send_raw(CMD_READ, path.encode("utf-8"))

        # First response: RESP_OK with 4-byte file size
        pkt_type, payload = self._recv_packet(timeout=10)
        if pkt_type == RESP_ERROR:
            msg = payload.decode("utf-8", errors="replace")
            raise ESP32CamError(f"READ failed: {msg}")
        if pkt_type != RESP_OK:
            raise ESP32CamError(f"Expected RESP_OK, got 0x{pkt_type:02x}")
        if len(payload) < 4:
            raise ESP32CamError("READ response missing file size")

        file_size = struct.unpack("<I", payload[:4])[0]
        self._log(f"  File size: {file_size} bytes")

        result = bytearray()
        while len(result) < file_size:
            pkt_type, payload = self._recv_packet(timeout=10)
            if pkt_type == DATA_CHUNK:
                result.extend(payload)
                self._log(f"  Chunk received {len(payload)} bytes (total {len(result)}/{file_size})")
                # ACK the chunk so board sends the next one
                self._send_raw(RESP_OK, b"")
            elif pkt_type == END_OF_DATA:
                self._log("  END_OF_DATA received early")
                break
            elif pkt_type == RESP_ERROR:
                msg = payload.decode("utf-8", errors="replace")
                raise ESP32CamError(f"READ error: {msg}")
            else:
                self._log(f"  Unexpected packet 0x{pkt_type:02x}")

        # Final END_OF_DATA
        if len(result) >= file_size:
            pkt_type, _ = self._recv_packet(timeout=5)
            if pkt_type == END_OF_DATA:
                self._send_raw(RESP_OK, b"")  # final ACK
                self._log("  END_OF_DATA ACKed")
            else:
                self._log(f"  Expected END_OF_DATA, got 0x{pkt_type:02x}")

        self._log(f"  Downloaded {len(result)} bytes")
        return bytes(result)

    def store_file(self, path: str, data: bytes) -> bool:
        """Upload a file to the device — stop-and-wait per chunk."""
        size = len(data)
        self._log(f"store_file('{path}', {size} bytes)")

        path_bytes = path.encode("utf-8")
        if len(path_bytes) > 124:
            raise ESP32CamError("Path too long (max 124 bytes)")

        payload = bytearray(124 + 4)
        payload[: len(path_bytes)] = path_bytes
        payload[124:128] = struct.pack("<I", size)

        self._send_raw(CMD_STORE, bytes(payload))

        # Wait for Ready
        pkt_type, payload = self._recv_packet(timeout=10)
        if pkt_type == RESP_ERROR:
            msg = payload.decode("utf-8", errors="replace")
            raise ESP32CamError(f"STORE rejected: {msg}")
        if pkt_type != RESP_OK:
            raise ESP32CamError(f"Expected RESP_OK, got 0x{pkt_type:02x}")
        self._log("  Device ready, sending chunks...")

        offset = 0
        chunk_num = 0
        while offset < size:
            chunk_num += 1
            chunk = data[offset : offset + _FILE_CHUNK_SIZE]
            self._send_raw(DATA_CHUNK, chunk)
            offset += len(chunk)
            self._log(f"  Chunk {chunk_num}: sent {len(chunk)} bytes ({offset}/{size})")

            # Wait for board ACK before sending next chunk
            ack_type, ack_payload = self._recv_packet(timeout=10)
            if ack_type == RESP_ERROR:
                msg = ack_payload.decode("utf-8", errors="replace")
                raise ESP32CamError(f"Chunk {chunk_num} rejected: {msg}")
            if ack_type != RESP_OK:
                self._log(f"  Chunk {chunk_num}: unexpected ACK 0x{ack_type:02x}")

        self._send_raw(END_OF_DATA, b"")
        self._log("  Sent END_OF_DATA")

        # Final response
        pkt_type, payload = self._recv_packet(timeout=10)
        if pkt_type == RESP_OK:
            msg = payload.decode("utf-8", errors="replace")
            self._log(f"  Upload OK: {msg}")
            return True
        if pkt_type == RESP_ERROR:
            msg = payload.decode("utf-8", errors="replace")
            raise ESP32CamError(f"Upload failed: {msg}")
        raise ESP32CamError(f"Unexpected response: 0x{pkt_type:02x}")

    def delete_file(self, path: str) -> bool:
        """Delete a file on the device."""
        self._log(f"delete_file('{path}')")
        self._send_raw(CMD_DELETE, path.encode("utf-8"))

        pkt_type, payload = self._recv_packet(timeout=10)
        if pkt_type == RESP_OK:
            self._log("  Delete OK")
            return True
        if pkt_type == RESP_ERROR:
            msg = payload.decode("utf-8", errors="replace")
            raise ESP32CamError(f"Delete failed: {msg}")
        raise ESP32CamError(f"Unexpected response: 0x{pkt_type:02x}")

    def switch_mode(self, mode: str) -> bool:
        """Switch the device mode (``photo`` or ``transfer``)."""
        mode_byte = MODE_PHOTO if mode.lower() == "photo" else MODE_TRANSFER
        self._log(f"switch_mode('{mode}') -> 0x{mode_byte:02x}")
        self._send_raw(CMD_MODE, bytes([mode_byte]))

        pkt_type, payload = self._recv_packet(timeout=10)
        if pkt_type == RESP_OK:
            self._log("  Mode switch acknowledged")
            if mode_byte == MODE_PHOTO:
                self._log("  Device entering deep sleep, disconnecting")
                self.disconnect()
            return True
        if pkt_type == RESP_ERROR:
            msg = payload.decode("utf-8", errors="replace")
            raise ESP32CamError(f"Mode switch failed: {msg}")
        raise ESP32CamError(f"Unexpected response: 0x{pkt_type:02x}")

    # ------------------------------------------------------------------
    # REPL
    # ------------------------------------------------------------------
    def repl(self) -> None:
        """Interactive pass-through REPL."""
        print(f"Connected to {self.port} at {self.baud} baud.")
        print("Type '!quit' to exit. Prefix with '!' for local helpers.")
        print()

        while True:
            try:
                user_input = input("esp32> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not user_input:
                continue
            if user_input in ("!quit", "!exit"):
                break
            if user_input.startswith("!"):
                self._handle_local(user_input)
                continue

            # Send raw text as a PING-like packet?  Not supported by firmware.
            print("Text commands are not supported in SerialTransfer mode.")
            print("Use !list, !upload, !download, !delete instead.")

    def _handle_local(self, cmd: str) -> None:
        parts = cmd.split(None, 2)
        local = parts[0][1:]  # strip leading '!'

        if local == "list":
            path = parts[1] if len(parts) > 1 else "/sdcard"
            try:
                for e in self.list_files(path):
                    if e["type"] == "dir":
                        print(f"  [DIR]  {e['name']}")
                    else:
                        print(f"  [FILE] {e['size']:>8} bytes  {e['name']}")
            except ESP32CamError as e:
                print(f"Error: {e}")

        elif local == "delete":
            if len(parts) < 2:
                print("Usage: !delete <path>")
                return
            try:
                self.delete_file(parts[1])
                print(f"Deleted: {parts[1]}")
            except ESP32CamError as e:
                print(f"Error: {e}")

        elif local == "upload":
            if len(parts) < 3:
                print("Usage: !upload <local_path> <remote_path>")
                return
            try:
                with open(parts[1], "rb") as f:
                    data = f.read()
                self.store_file(parts[2], data)
                print(f"Uploaded {parts[1]} -> {parts[2]} ({len(data)} bytes)")
            except FileNotFoundError:
                print(f"File not found: {parts[1]}")
            except ESP32CamError as e:
                print(f"Error: {e}")

        elif local == "download":
            if len(parts) < 3:
                print("Usage: !download <remote_path> <local_path>")
                return
            try:
                data = self.read_file(parts[1])
                with open(parts[2], "wb") as f:
                    f.write(data)
                print(f"Downloaded {parts[1]} -> {parts[2]} ({len(data)} bytes)")
            except ESP32CamError as e:
                print(f"Error: {e}")

        elif local == "mode":
            if len(parts) < 2:
                print("Usage: !mode <photo|transfer>")
                return
            try:
                self.switch_mode(parts[1])
                print(f"Mode switched to {parts[1]}")
            except ESP32CamError as e:
                print(f"Error: {e}")

        else:
            print(f"Unknown local command: {local}")


# =============================================================================
# CLI / TUI
# =============================================================================
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ESP32-CAM Serial File Transfer Client (SerialTransfer)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s /dev/ttyACM0 repl
  %(prog)s -v /dev/ttyACM0 list /sdcard
  %(prog)s /dev/ttyACM0 upload photo.jpg /picture99.jpg
  %(prog)s /dev/ttyACM0 download /picture1.jpg ./saved.jpg
  %(prog)s /dev/ttyACM0 mode photo
""",
    )
    p.add_argument("port", help="Serial port (e.g. /dev/ttyACM0, COM3)")
    p.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose debug output",
    )
    sub = p.add_subparsers(dest="command", help="Command to run")

    sub.add_parser("repl", help="Interactive REPL")

    p_list = sub.add_parser("list", help="List files")
    p_list.add_argument("path", nargs="?", default="/sdcard", help="Directory path")

    p_del = sub.add_parser("delete", help="Delete a file")
    p_del.add_argument("path", help="File path on device")

    p_up = sub.add_parser("upload", help="Upload a file to the device")
    p_up.add_argument("src", help="Local file path")
    p_up.add_argument("dst", help="Remote file path on device")

    p_down = sub.add_parser("download", help="Download a file from the device")
    p_down.add_argument("src", help="Remote file path on device")
    p_down.add_argument("dst", help="Local file path")

    p_mode = sub.add_parser("mode", help="Switch device mode")
    p_mode.add_argument("mode", choices=["photo", "transfer"], help="Target mode")

    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    cam = ESP32CamClient(args.port, verbose=args.verbose)

    print(f"[tui] Connecting to {args.port}...")
    try:
        cam.connect()
        print(f"[tui] Connected at {cam.baud} baud.")
    except ConnectionError as e:
        print(f"[tui] Failed to connect: {e}")
        sys.exit(1)

    try:
        if args.command == "repl":
            cam.repl()

        elif args.command == "list":
            print(f"[tui] Listing '{args.path}'...")
            entries = cam.list_files(args.path)
            print(f"[tui] {len(entries)} entries found")
            for e in entries:
                if e["type"] == "dir":
                    print(f"[DIR]  {e['name']}")
                else:
                    print(f"[FILE] {e['size']:>8} bytes  {e['name']}")

        elif args.command == "delete":
            print(f"[tui] Deleting '{args.path}'...")
            cam.delete_file(args.path)
            print(f"[tui] Deleted: {args.path}")

        elif args.command == "upload":
            print(f"[tui] Reading local file '{args.src}'...")
            with open(args.src, "rb") as f:
                data = f.read()
            print(f"[tui] Uploading {len(data)} bytes -> '{args.dst}'...")
            cam.store_file(args.dst, data)
            print(f"[tui] Uploaded {args.src} -> {args.dst} ({len(data)} bytes)")

        elif args.command == "download":
            print(f"[tui] Downloading '{args.src}' -> '{args.dst}'...")
            data = cam.read_file(args.src)
            with open(args.dst, "wb") as f:
                f.write(data)
            print(f"[tui] Downloaded {args.src} -> {args.dst} ({len(data)} bytes)")

        elif args.command == "mode":
            print(f"[tui] Switching to {args.mode} mode...")
            cam.switch_mode(args.mode)
            print(f"[tui] Mode switched to {args.mode}")

    except ESP32CamError as e:
        print(f"[tui] Device error: {e}")
        sys.exit(1)
    except FileNotFoundError as e:
        print(f"[tui] File not found: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[tui] Interrupted.")
    finally:
        print("[tui] Disconnecting...")
        cam.disconnect()
        print("[tui] Done.")


if __name__ == "__main__":
    main()
