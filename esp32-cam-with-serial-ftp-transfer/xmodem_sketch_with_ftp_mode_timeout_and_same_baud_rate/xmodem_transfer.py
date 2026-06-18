"""ESP32-CAM Serial File Transfer Library (XMODEM-CRC).

Uses the ``xmodem`` PyPI package for the XMODEM-CRC protocol and
``pyserial`` for serial I/O.

Dependencies
------------
- pyserial >= 3.5
- xmodem >= 0.4.7

Install::

    pip install -r requirements.txt
"""

from __future__ import annotations

import io
import re
import time

import serial
from xmodem import XMODEM

CHUNK_SIZE = 128  # must match xmodem-lib data size on the Arduino


class ESP32CamError(Exception):
    """Raised when the device responds with an error or a protocol mismatch."""


class ESP32Serial:
    """High-level serial client for ESP32-CAM XMODEM firmware.

    Two-phase connection:
        1. Try *transfer_baud* and send HELP.
        2. Fall back to 115_200 baud, wait for boot prompt, send MODE TRANSFER,
           then reconnect at *transfer_baud*.
    """

    def __init__(
        self,
        port: str,
        transfer_baud: int = 921600,
        timeout: int = 30,
        verbose: bool = False,
    ):
        self.port = port
        self.transfer_baud = transfer_baud
        self.timeout = timeout
        self.verbose = verbose
        self.ser: serial.Serial | None = None
        self.baud: int | None = None
        self._line_buf: bytes = b""

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[lib] {msg}")

    def _drain(self) -> None:
        """Discard everything currently in the serial input buffer."""
        if self.ser and self.ser.is_open:
            self.ser.reset_input_buffer()
            self._line_buf = b""

    def _read_until_prompt(self, timeout: float = 10.0) -> str:
        """Read bytes until the ``> `` prompt appears and return the text before it."""
        buf = self._line_buf
        start = time.time()
        while time.time() - start < timeout:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting)
                buf += chunk
                if b"> " in buf:
                    idx = buf.rfind(b"> ")
                    result = buf[:idx].decode("utf-8", errors="replace").strip()
                    self._line_buf = buf[idx + 2:]
                    return result
            time.sleep(0.02)
        self._line_buf = b""
        return buf.decode("utf-8", errors="replace").strip()

    def _send_text(self, text: str) -> str:
        """Send a text command and read back until the next prompt."""
        if not self.ser or not self.ser.is_open:
            raise ESP32CamError("Not connected")
        self._log(f"_send_text: {text!r}")
        self.ser.write(f"{text}\n".encode())
        self.ser.flush()
        response = self._read_until_prompt()
        self._log(f"  -> {len(response)} chars")
        if self.verbose and response:
            preview = response[:200].replace("\n", "\\n")
            self._log(f"  preview: {preview!r}")
        return response

    # XMODEM callbacks bound to our serial port
    def _xmodem_getc(self, size: int, timeout: float = 1.0) -> bytes | None:
        """XMODEM getc callback."""
        start = time.time()
        data = b""
        while len(data) < size and time.time() - start < timeout:
            if self.ser.in_waiting:
                chunk = self.ser.read(min(size - len(data), self.ser.in_waiting))
                if chunk:
                    data += chunk
            if len(data) < size:
                time.sleep(0.001)
        return data if data else None

    def _xmodem_putc(self, data: bytes, timeout: float = 1.0) -> int:
        """XMODEM putc callback."""
        return self.ser.write(data)

    # ------------------------------------------------------------------
    # Connection
    # ------------------------------------------------------------------

    def connect(self) -> bool:
        """Establish connection using the two-phase protocol."""
        self._log(
            f"connect() port={self.port} transfer_baud={self.transfer_baud} "
            f"timeout={self.timeout}"
        )

        self._log(f"Phase 1: trying {self.transfer_baud} baud...")
        if self._try_baud(self.transfer_baud):
            self._log(f"Connected at {self.baud} baud")
            return True

        self._log("Phase 2: fallback to 115_200 baud...")
        if self._try_fallback():
            self._log(f"Connected at {self.baud} baud after fallback")
            return True

        raise ConnectionError(f"Could not connect to {self.port}")

    def disconnect(self) -> None:
        self._log("disconnect()")
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None

    def _try_baud(self, baud: int) -> bool:
        try:
            self.ser = serial.Serial(self.port, baud, timeout=2)
            time.sleep(0.2)
            self.ser.reset_input_buffer()

            self.ser.write(b"HELP\n")
            self.ser.flush()
            self._log(f"  HELP sent at {baud}")

            response = b""
            start = time.time()
            while time.time() - start < 5:
                if self.ser.in_waiting:
                    chunk = self.ser.read(self.ser.in_waiting)
                    response += chunk
                    if b"Available commands:" in response:
                        self.baud = baud
                        self._read_until_prompt(timeout=2.0)
                        return True
                time.sleep(0.05)

            self.ser.close()
            self.ser = None
            return False
        except Exception as exc:
            self._log(f"  Exception: {type(exc).__name__}: {exc}")
            if self.ser:
                self.ser.close()
                self.ser = None
            return False

    def _try_fallback(self) -> bool:
        try:
            self.ser = serial.Serial(self.port, 115_200, timeout=1)
            time.sleep(0.2)
            self.ser.reset_input_buffer()

            response = b""
            start = time.time()
            while time.time() - start < self.timeout:
                if self.ser.in_waiting:
                    chunk = self.ser.read(self.ser.in_waiting)
                    response += chunk
                    if b"Send 'MODE TRANSFER' to switch" in response:
                        self._log("  Boot prompt detected, sending MODE TRANSFER")
                        self.ser.write(b"MODE TRANSFER\n")
                        self.ser.flush()
                        time.sleep(1)
                        self.ser.close()
                        self.ser = None
                        time.sleep(0.5)
                        return self._try_baud(self.transfer_baud)
                time.sleep(0.1)

            self.ser.close()
            self.ser = None
            return False
        except Exception as exc:
            self._log(f"  Exception: {type(exc).__name__}: {exc}")
            if self.ser:
                self.ser.close()
                self.ser = None
            return False

    # ------------------------------------------------------------------
    # Commands
    # ------------------------------------------------------------------

    def list_files(self, path: str = "/sdcard") -> list[dict]:
        """List directory contents."""
        self._log(f"list_files({path!r})")
        response = self._send_text(f"LIST {path}")
        entries: list[dict] = []
        for line in response.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith("[DIR]"):
                name = line[5:].strip()
                entries.append({"type": "dir", "name": name})
            elif line.startswith("[FILE]"):
                rest = line[6:].strip()
                match = re.match(r"(\d+)\s+bytes\s+(.+)", rest)
                if match:
                    entries.append({
                        "type": "file",
                        "size": int(match.group(1)),
                        "name": match.group(2).strip(),
                    })
                else:
                    parts = rest.split(None, 2)
                    if len(parts) >= 3 and parts[1].lower() == "bytes":
                        entries.append({
                            "type": "file",
                            "size": int(parts[0]),
                            "name": parts[2],
                        })
        self._log(f"  {len(entries)} entries")
        return entries

    def delete_file(self, path: str) -> bool:
        """Delete a file on the device."""
        self._log(f"delete_file({path!r})")
        response = self._send_text(f"DELETE {path}")
        if "[OK]" in response:
            return True
        if "[ERROR]" in response:
            raise ESP32CamError(response.strip())
        return False

    def read_file(self, path: str) -> bytes:
        """Download a file from the device via XMODEM-CRC."""
        self._log(f"read_file({path!r})")
        if not self.ser or not self.ser.is_open:
            raise ESP32CamError("Not connected")

        self._drain()
        self.ser.write(f"READ {path}\n".encode())
        self.ser.flush()

        # Read until we see the XMODEM header or an error
        header = b""
        start = time.time()
        while time.time() - start < 10:
            if self.ser.in_waiting:
                header += self.ser.read(self.ser.in_waiting)
                if b"\n" in header:
                    break
            time.sleep(0.02)

        header_str = header.decode("utf-8", errors="replace")
        self._log(f"  header: {header_str.strip()!r}")

        match = re.search(r"XMODEM\s+(\d+)", header_str)
        if not match:
            rest = self._read_until_prompt()
            full = header_str + "\n" + rest
            if "[ERROR]" in full:
                raise ESP32CamError(full.strip())
            raise ESP32CamError(f"Cannot parse XMODEM size from: {full.strip()!r}")

        file_size = int(match.group(1))
        self._log(f"  file size: {file_size}")

        # Save any trailing bytes from the header read into line buffer
        nl_idx = header.find(b"\n")
        if nl_idx != -1:
            self._line_buf = header[nl_idx + 1:]

        # Discard any stray bytes that arrived after the newline
        self._drain()

        # XMODEM receive
        stream = io.BytesIO()
        modem = XMODEM(self._xmodem_getc, self._xmodem_putc)
        self._log("  starting XMODEM recv...")
        ok = modem.recv(stream, crc_mode=0, retry=16, timeout=10)
        if not ok:
            raise ESP32CamError("XMODEM receive failed")

        data = stream.getvalue()
        self._log(f"  received {len(data)} bytes")

        # Trim to actual file size (XMODEM pads last block with 0x1A)
        data = data[:file_size]

        # Consume trailing prompt
        self._read_until_prompt(timeout=3.0)
        return data

    def store_file(self, path: str, data: bytes) -> bool:
        """Upload a file to the device via XMODEM-CRC."""
        size = len(data)
        self._log(f"store_file({path!r}, {size} bytes)")
        if not self.ser or not self.ser.is_open:
            raise ESP32CamError("Not connected")

        self._drain()
        self.ser.write(f"STORE {path} {size}\n".encode())
        self.ser.flush()

        # Wait for XMODEM READY or error
        response = b""
        start = time.time()
        while time.time() - start < 10:
            if self.ser.in_waiting:
                response += self.ser.read(self.ser.in_waiting)
                if b"\n" in response:
                    break
            time.sleep(0.02)

        resp_str = response.decode("utf-8", errors="replace")
        self._log(f"  response: {resp_str.strip()!r}")

        if "XMODEM READY" not in resp_str:
            rest = self._read_until_prompt()
            full = resp_str + "\n" + rest
            if "[ERROR]" in full:
                raise ESP32CamError(full.strip())
            raise ESP32CamError(f"Unexpected STORE response: {full.strip()!r}")

        # Save trailing bytes
        nl_idx = response.find(b"\n")
        if nl_idx != -1:
            self._line_buf = response[nl_idx + 1:]

        # Discard any stray bytes that arrived after the newline
        # (e.g. Arduino debug prints or prompt chars on the same Serial line)
        self._drain()

        # XMODEM send
        stream = io.BytesIO(data)
        modem = XMODEM(self._xmodem_getc, self._xmodem_putc)
        self._log("  starting XMODEM send...")
        ok = modem.send(stream, retry=16, timeout=10)
        if not ok:
            raise ESP32CamError("XMODEM send failed")

        self._log("  XMODEM send complete")

        # Read final prompt response
        post = self._read_until_prompt(timeout=5.0)
        self._log(f"  post-upload: {post[:150]!r}")
        if "[OK]" in post:
            return True
        if "[ERROR]" in post:
            raise ESP32CamError(post.strip())
        return False

    def switch_mode(self, mode: str) -> bool:
        """Switch device mode (``photo`` or ``transfer``)."""
        mode_u = mode.upper()
        self._log(f"switch_mode({mode_u!r})")
        response = self._send_text(f"MODE {mode_u}")
        if "[OK]" in response:
            if mode_u == "PHOTO":
                self.disconnect()
            return True
        if "[ERROR]" in response:
            raise ESP32CamError(response.strip())
        return False

    # ------------------------------------------------------------------
    # REPL
    # ------------------------------------------------------------------

    def repl(self) -> None:
        """Interactive pass-through REPL."""
        print(f"Connected to {self.port} at {self.baud} baud.")
        print("Type '!quit' to exit.")
        print("Prefix with '!' for local helpers (!list, !upload, !download, !delete)")
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

            try:
                response = self._send_text(user_input)
                if response:
                    print(response)
            except ESP32CamError as e:
                print(f"Error: {e}")

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

        else:
            print(f"Unknown local command: {local}")
