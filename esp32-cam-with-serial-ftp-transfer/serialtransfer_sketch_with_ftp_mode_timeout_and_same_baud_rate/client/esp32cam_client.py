#!/usr/bin/env python3
"""ESP32-CAM Dual-Mode Serial Client

Connects to the ESP32-CAM at a unified 921600 baud rate using a plain-text
protocol. The client queries the current mode and switches to TRANSFER if
needed.

Dependencies:
    pip install pyserial>=3.5
"""

from __future__ import annotations

import argparse
import os
import sys
import time

import serial

# ---------------------------------------------------------------------------
# Connection constants — must match the Arduino sketch
# ---------------------------------------------------------------------------
BAUD_RATE = 921600
INIT_TIMEOUT = 10.0       # seconds to wait for mode switch response
RESPONSE_IDLE_MS = 200    # ms of silence → response is complete
EOT = b"\x04"


class ESP32CamError(Exception):
    """Raised when the device responds with an error or protocol mismatch."""


class ESP32CamClient:
    """High-level serial client for the ESP32-CAM dual-mode firmware."""

    def __init__(self, port: str, timeout: float = 30.0, verbose: bool = False):
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        self.ser: serial.Serial | None = None

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
        """Connect at BAUD_RATE, query mode, switch to TRANSFER if needed."""
        self._log(f"connect() port={self.port} baud={BAUD_RATE}")

        # 1. Open serial port
        try:
            self.ser = serial.Serial(self.port, baudrate=BAUD_RATE, timeout=1.0)
        except serial.SerialException as e:
            raise ConnectionError(f"Cannot open {self.port}: {e}")

        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self._log("Port opened, input buffer reset")

        # 2. Query current mode
        self._log("Querying CURRENT_MODE...")
        self._send_cmd("CURRENT_MODE")
        response = self._read_response(timeout=3.0)
        self._log(f"CURRENT_MODE response: {response!r}")

        if "TRANSFER" in response:
            print("[OK] Board already in TRANSFER mode")
            return True

        # 3. Not in transfer mode — send MODE TRANSFER
        print("[INFO] Board not in TRANSFER mode, sending MODE TRANSFER...")
        self._send_cmd("MODE TRANSFER")

        # 4. Wait up to INIT_TIMEOUT for confirmation
        #    Collect FULL buffer, display received bytes for debugging.
        full_buffer = ""
        start = time.time()
        while time.time() - start < INIT_TIMEOUT:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
                full_buffer += chunk
                # Display each line as it arrives for debugging
                for line in chunk.splitlines():
                    print(f"  [RX] {line}")
            if "[OK] mode changed to TRANSFER" in full_buffer:
                print("[OK] Mode switched to TRANSFER")
                return True
            time.sleep(0.05)

        # 5. Timeout
        print(f"[ERROR] Mode switch timeout ({INIT_TIMEOUT}s)")
        print(f"[DEBUG] Full buffer received:\n{full_buffer}")
        raise ConnectionError("Failed to switch board to TRANSFER mode")

    def disconnect(self) -> None:
        if self.ser:
            self.ser.close()
            self.ser = None
        self._log("Disconnected")

    # ------------------------------------------------------------------
    # Low-level I/O
    # ------------------------------------------------------------------
    def _send_cmd(self, cmd: str) -> None:
        """Send a text command with newline."""
        if not self.ser:
            raise ESP32CamError("Not connected")
        data = (cmd + "\n").encode("utf-8")
        self.ser.write(data)
        self.ser.flush()
        self._log(f"TX: {cmd!r}")

    def _drain_stale(self) -> bytes:
        """Non-blocking drain of stale data in the serial buffer.

        Returns drained bytes.  NOT called during init (per spec).
        Reference: bt_ftp_client.py _drain_stale()
        """
        if not self.ser:
            return b""
        drained = b""
        for _ in range(20):          # poll up to 200ms
            avail = self.ser.in_waiting
            if avail > 0:
                chunk = self.ser.read(min(4096, avail))
                drained += chunk
            elif drained:
                break
            time.sleep(0.01)

        if drained:
            decoded = drained.decode("utf-8", errors="replace").rstrip()
            print(f"[WARN] Drained {len(drained)} stale bytes:")
            for line in decoded.splitlines():
                print(f"       | {line}")
        return drained

    def _read_line(self, timeout: float = 5.0) -> str:
        """Read bytes until '\n' and return the decoded line (without newline)."""
        if not self.ser:
            raise ESP32CamError("Not connected")
        buf = b""
        start = time.time()
        while b"\n" not in buf:
            if self.ser.in_waiting > 0:
                buf += self.ser.read(1)
            elif time.time() - start > timeout:
                break
            else:
                time.sleep(0.005)
        line = buf.decode("utf-8", errors="replace").rstrip("\r\n")
        self._log(f"  LINE: {line!r}")
        return line

    def _read_response(self, timeout: float = 10.0) -> str:
        """Read text response until idle for RESPONSE_IDLE_MS.

        Returns the decoded text (without trailing prompt).
        """
        if not self.ser:
            raise ESP32CamError("Not connected")

        buf = b""
        empty_rounds = 0
        start = time.time()

        while True:
            avail = self.ser.in_waiting
            if avail > 0:
                chunk = self.ser.read(min(4096, avail))
                buf += chunk
                empty_rounds = 0
                self._log(f"  RX {len(chunk)} bytes (total {len(buf)})")
            else:
                empty_rounds += 1
                if empty_rounds >= 20:          # ~200ms idle
                    self._log("  Response complete (idle)")
                    break
                if time.time() - start > timeout:
                    self._log(f"  Response timeout ({timeout}s)")
                    break
                time.sleep(0.01)

        text = buf.decode("utf-8", errors="replace")
        self._log(f"  Response text ({len(text)} chars): {text!r}")
        return text

    # ------------------------------------------------------------------
    # Commands
    # ------------------------------------------------------------------
    def current_mode(self) -> str:
        """Query the current device mode."""
        self._drain_stale()
        self._send_cmd("CURRENT_MODE")
        resp = self._read_response(timeout=3.0)
        for line in resp.splitlines():
            if "mode:" in line:
                return line.split(":", 1)[1].strip()
        return "UNKNOWN"

    def list_files(self, path: str = "/") -> list[dict]:
        """List directory contents."""
        self._drain_stale()
        self._send_cmd(f"LIST {path}")
        resp = self._read_response(timeout=10.0)

        entries: list[dict] = []
        for line in resp.splitlines():
            line = line.strip()
            if line.startswith("[FILE]"):
                parts = line[6:].strip().split(None, 1)
                if len(parts) == 2:
                    entries.append({
                        "type": "file",
                        "size": int(parts[0]),
                        "name": parts[1],
                    })
            elif line.startswith("[DIR]"):
                name = line[5:].strip()
                entries.append({"type": "dir", "name": name})
            elif line == "[OK]":
                break
        return entries

    def read_file(self, path: str) -> bytes:
        """Download a file from the device."""
        self._drain_stale()
        self._send_cmd(f"READ {path}")

        # Read SIZE: header line (line-based to avoid pulling binary data)
        header_line = self._read_line(timeout=5.0)
        if not header_line.startswith("SIZE:"):
            raise ESP32CamError(f"Expected SIZE header, got: {header_line!r}")

        file_size = int(header_line.replace("SIZE:", "").strip())
        self._log(f"File size: {file_size}")

        # Read exactly file_size bytes
        data = b""
        start = time.time()
        while len(data) < file_size:
            needed = file_size - len(data)
            chunk = self.ser.read(min(4096, needed))
            if not chunk:
                if time.time() - start > 30.0:
                    raise ESP32CamError(f"Timeout reading file data. Got {len(data)}/{file_size}")
                time.sleep(0.01)
                continue
            data += chunk

        # Consume EOT byte
        eot = self.ser.read(1)
        if eot != EOT:
            self._log(f"Expected EOT, got: {eot!r}")

        # Drain trailing response ([OK])
        self._read_response(timeout=2.0)

        self._log(f"Downloaded {len(data)} bytes")
        return data

    def store_file(self, path: str, data: bytes) -> bool:
        """Upload a file to the device."""
        size = len(data)
        self._drain_stale()
        self._send_cmd(f"STORE {path} {size}")

        # Wait for "[OK] Ready to receive"
        resp = self._read_response(timeout=5.0)
        if "[OK] Ready to receive" not in resp:
            raise ESP32CamError(f"STORE rejected: {resp}")

        self._log("Device ready, sending data...")

        # Send data in paced chunks to avoid overflowing ESP32 serial buffer
        CHUNK = 512
        offset = 0
        while offset < size:
            end = min(offset + CHUNK, size)
            self.ser.write(data[offset:end])
            self.ser.flush()
            offset = end
            time.sleep(0.02)  # give board time to read from UART FIFO

        self._log("All data sent, waiting for confirmation...")

        # Wait for final [OK] Stored
        resp = self._read_response(timeout=30.0)
        if "[OK] Stored" in resp:
            self._log("Upload confirmed")
            return True
        if "[ERROR]" in resp:
            raise ESP32CamError(f"Upload failed: {resp}")
        # Fallback: check for any ACK lines
        self._log(f"Final response: {resp!r}")
        return True

    def delete_file(self, path: str) -> bool:
        """Delete a file on the device."""
        self._drain_stale()
        self._send_cmd(f"DELETE {path}")
        resp = self._read_response(timeout=5.0)
        if "[OK]" in resp:
            return True
        raise ESP32CamError(f"Delete failed: {resp}")

    def switch_mode(self, mode: str) -> bool:
        """Switch the device mode (photo or transfer)."""
        cmd = f"MODE {mode.upper()}"
        self._drain_stale()
        self._send_cmd(cmd)
        resp = self._read_response(timeout=5.0)
        if "[OK]" in resp:
            return True
        raise ESP32CamError(f"Mode switch failed: {resp}")

    # ------------------------------------------------------------------
    # REPL
    # ------------------------------------------------------------------
    def repl(self) -> None:
        """Interactive pass-through REPL."""
        print(f"Connected to {self.port} at {BAUD_RATE} baud.")
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

            # Send raw text command
            self._drain_stale()
            self._send_cmd(user_input)
            resp = self._read_response(timeout=10.0)
            print(resp)

    def _handle_local(self, cmd: str) -> None:
        parts = cmd.split(None, 2)
        local = parts[0][1:]  # strip leading '!'

        if local == "list":
            path = parts[1] if len(parts) > 1 else "/"
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

        elif local == "current":
            try:
                mode = self.current_mode()
                print(f"Current mode: {mode}")
            except ESP32CamError as e:
                print(f"Error: {e}")

        else:
            print(f"Unknown local command: {local}")


# =============================================================================
# CLI / TUI
# =============================================================================
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ESP32-CAM Dual-Mode Serial Client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s /dev/ttyUSB0 repl
  %(prog)s -v /dev/ttyUSB0 list /
  %(prog)s /dev/ttyUSB0 upload photo.jpg /picture99.jpg
  %(prog)s /dev/ttyUSB0 download /picture1.jpg ./saved.jpg
  %(prog)s /dev/ttyUSB0 mode photo
  %(prog)s /dev/ttyUSB0 current
""",
    )
    p.add_argument("port", help="Serial port (e.g. /dev/ttyUSB0, COM3)")
    p.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose debug output",
    )
    sub = p.add_subparsers(dest="command", help="Command to run")

    sub.add_parser("repl", help="Interactive REPL")
    sub.add_parser("current", help="Query current mode")

    p_list = sub.add_parser("list", help="List files")
    p_list.add_argument("path", nargs="?", default="/", help="Directory path")

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
    except (ConnectionError, ESP32CamError) as e:
        print(f"[tui] Failed to connect: {e}")
        sys.exit(1)

    try:
        if args.command == "repl":
            cam.repl()

        elif args.command == "current":
            mode = cam.current_mode()
            print(f"[tui] Current mode: {mode}")

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
