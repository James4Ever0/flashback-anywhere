"""ESP32-CAM Serial FTP Library.

Handles two-phase connection, command/response protocol, and file operations
over a serial link to the CombinedSketch firmware.
"""

import re
import time

import serial

CHUNK_SIZE = 2048


def compute_checksum(data: bytes) -> int:
    """djb2 hash — must match ESP32 firmware computeChecksum()."""
    h = 5381
    for b in data:
        h = ((h << 5) + h) + b
        h &= 0xFFFFFFFF
    return h


class ESP32CamError(Exception):
    """Raised when the device responds with an error or protocol mismatch."""


class ESP32CamSerial:
    """High-level serial client for ESP32-CAM CombinedSketch."""

    def __init__(self, port: str, timeout: int = 30, verbose: bool = False):
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        self.ser: serial.Serial | None = None
        self.baud: int | None = None

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[lib] {msg}")

    # ---------- Connection ----------

    def connect(self) -> bool:
        """Connect using the two-phase protocol.

        1. Try 2 000 000 baud and send HELP.
        2. Fall back to 115 200 baud, wait for boot prompt, send MODE TRANSFER,
           then reconnect at 2 000 000 baud.
        """
        self._log(f"connect() called, port={self.port}, timeout={self.timeout}")

        self._log("Phase 1: trying 2 000 000 baud...")
        if self._try_baud(2_000_000):
            self._log(f"Connected successfully at {self.baud} baud")
            return True

        self._log("Phase 1 failed. Phase 2: trying 115 200 baud fallback...")
        if self._try_fallback():
            self._log(f"Connected successfully at {self.baud} baud after fallback")
            return True

        self._log("All connection attempts failed")
        raise ConnectionError(f"Could not connect to {self.port}")

    def disconnect(self) -> None:
        self._log("disconnect() called")
        if self.ser and self.ser.is_open:
            self._log(f"Closing serial port {self.port}")
            self.ser.close()
        self.ser = None
        self._log("Disconnected")

    def _try_baud(self, baud: int) -> bool:
        self._log(f"_try_baud({baud}): opening serial port...")
        try:
            self.ser = serial.Serial(self.port, baud, timeout=2)
            self._log(f"  Port opened: {self.ser.name}")
            time.sleep(0.2)
            self.ser.reset_input_buffer()
            self._log("  Input buffer flushed")

            self._log("  Sending HELP...")
            self.ser.write(b"HELP\n")
            self.ser.flush()
            self._log("  HELP sent, waiting for response (5s timeout)...")

            response = b""
            start = time.time()
            while time.time() - start < 5:
                if self.ser.in_waiting:
                    chunk = self.ser.read(self.ser.in_waiting)
                    response += chunk
                    self._log(f"  Received {len(chunk)} bytes (total {len(response)})")
                    if b"Available commands:" in response:
                        self.baud = baud
                        self._log(f"  Found 'Available commands:' marker")
                        if b"> " in response:
                            self._log("  Found prompt marker")
                            return True
                        self._log("  Reading until prompt...")
                        prompt_text = self._read_until_prompt()
                        self._log(f"  Prompt text length: {len(prompt_text)} chars")
                        return True
                time.sleep(0.05)

            self._log(f"  Timeout. Response preview: {response[:200]!r}")
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
        self._log(f"_try_fallback(): opening at 115 200 baud, waiting up to {self.timeout}s for boot text...")
        try:
            self.ser = serial.Serial(self.port, 115_200, timeout=1)
            self._log(f"  Port opened at 115200")
            time.sleep(0.2)
            self.ser.reset_input_buffer()

            response = b""
            start = time.time()
            last_len = 0
            while time.time() - start < self.timeout:
                if self.ser.in_waiting:
                    chunk = self.ser.read(self.ser.in_waiting)
                    response += chunk
                    if len(response) != last_len:
                        self._log(f"  Received {len(chunk)} bytes (total {len(response)})")
                        last_len = len(response)
                    if b"Send 'MODE TRANSFER' to switch" in response:
                        self._log("  Found boot trigger text 'Send MODE TRANSFER to switch'")
                        self._log("  Sending MODE TRANSFER...")
                        self.ser.write(b"MODE TRANSFER\n")
                        self.ser.flush()
                        time.sleep(1)
                        self.ser.close()
                        self.ser = None
                        self._log("  Port closed, waiting 0.5s before reconnect at 2M...")
                        time.sleep(0.5)
                        return self._try_baud(2_000_000)
                time.sleep(0.1)

            self._log(f"  Fallback timeout after {self.timeout}s. Response preview: {response[:300]!r}")
            self.ser.close()
            self.ser = None
            return False
        except Exception as exc:
            self._log(f"  Exception: {type(exc).__name__}: {exc}")
            if self.ser:
                self.ser.close()
                self.ser = None
            return False

    # ---------- Low-level I/O ----------

    def _send_cmd(self, cmd: str) -> str:
        if not self.ser or not self.ser.is_open:
            raise ESP32CamError("Not connected")
        self._log(f"_send_cmd: '{cmd}'")
        self.ser.write(f"{cmd}\n".encode())
        self.ser.flush()
        response = self._read_until_prompt()
        self._log(f"  Response length: {len(response)} chars")
        if self.verbose and response:
            preview = response[:200].replace("\n", "\\n")
            self._log(f"  Response preview: {preview!r}")
        return response

    def _read_until_prompt(self) -> str:
        """Read until the ``> `` prompt appears.  Return text before it."""
        buf = b""
        start = time.time()
        while time.time() - start < 10:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting)
                buf += chunk
                if b"> " in buf:
                    idx = buf.rfind(b"> ")
                    result = buf[:idx].decode("utf-8", errors="replace").strip()
                    self._log(f"  Prompt detected after {len(buf)} bytes read")
                    return result
            time.sleep(0.05)
        self._log(f"  Prompt timeout after 10s. Read {len(buf)} bytes")
        return buf.decode("utf-8", errors="replace").strip()

    def _drain_bytes(self, count: int, timeout: float = 2.0) -> bytes:
        """Blocking read of exactly *count* bytes, retrying on short reads."""
        data = b""
        start = time.time()
        while len(data) < count:
            need = count - len(data)
            chunk = self.ser.read(need)
            if chunk:
                data += chunk
            if time.time() - start > timeout:
                raise ESP32CamError(
                    f"Drain timeout: got {len(data)}/{count} bytes"
                )
            time.sleep(0.001)
        return data

    def _read_line(self, timeout: int = 10, expected_prefix: str = "") -> str:
        """Read a line, skipping garbage until *expected_prefix* is found."""
        buf = b""
        start = time.time()
        prefix_b = expected_prefix.encode() if expected_prefix else b""
        while time.time() - start < timeout:
            if self.ser.in_waiting:
                buf += self.ser.read(self.ser.in_waiting)
            # If we have an expected prefix, search for it in the buffer
            if prefix_b and prefix_b in buf:
                idx = buf.find(prefix_b)
                rest = buf[idx:]
                nl = rest.find(b"\n")
                if nl != -1:
                    line = rest[:nl].decode("utf-8", errors="replace").strip()
                    self._log(f"  _read_line (prefixed): found '{line[:50]}' after skipping {idx} bytes")
                    buf = rest[nl + 1:]  # Save leftovers
                    self._line_buf = buf  # Store for next call if needed
                    return line
            # Otherwise just return the first complete line
            nl = buf.find(b"\n")
            if nl != -1:
                line = buf[:nl].decode("utf-8", errors="replace").strip()
                if line:
                    self._log(f"  _read_line: '{line[:50]}'")
                    buf = buf[nl + 1:]
                    self._line_buf = buf
                    return line
                buf = buf[nl + 1:]  # Empty line, keep looking
            time.sleep(0.01)
        # Timeout — return whatever we have
        return buf.decode("utf-8", errors="replace").strip()

    # ---------- Commands ----------

    def list_files(self, path: str = "/sdcard") -> list[dict]:
        """List directory contents."""
        self._log(f"list_files('{path}')")
        response = self._send_cmd(f"LIST {path}")
        entries: list[dict] = []
        for line in response.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith("[DIR]"):
                name = line[5:].strip()
                self._log(f"  Found dir: '{name}'")
                entries.append({"type": "dir", "name": name})
            elif line.startswith("[FILE]"):
                rest = line[6:].strip()
                match = re.match(r"(\d+)\s+bytes\s+(.+)", rest)
                if match:
                    size = int(match.group(1))
                    name = match.group(2).strip()
                    self._log(f"  Found file: '{name}' ({size} bytes)")
                    entries.append({"type": "file", "size": size, "name": name})
                else:
                    parts = rest.split(None, 2)
                    if len(parts) >= 3 and parts[1].lower() == "bytes":
                        size = int(parts[0])
                        name = parts[2]
                        self._log(f"  Found file (fallback): '{name}' ({size} bytes)")
                        entries.append({"type": "file", "size": size, "name": name})
        self._log(f"  Total entries: {len(entries)}")
        return entries

    def read_file(self, path: str) -> bytes:
        """Download a file from the device using chunked checksum protocol."""
        self._log(f"read_file('{path}')")
        if not self.ser or not self.ser.is_open:
            raise ESP32CamError("Not connected")

        self._log("  Sending READ command...")
        self.ser.write(f"READ {path}\n".encode())
        self.ser.flush()

        # Read SIZE header
        header = b""
        start = time.time()
        while time.time() - start < 10:
            if self.ser.in_waiting:
                header += self.ser.read(self.ser.in_waiting)
                if b"\n" in header:
                    break
            time.sleep(0.05)

        header_str = header.decode("utf-8", errors="replace")
        self._log(f"  Header: {header_str.strip()!r}")
        match = re.search(r"SIZE:\s*(\d+)", header_str)
        if not match:
            rest = self._read_until_prompt()
            full = header_str + "\n" + rest
            if "[ERROR]" in full:
                raise ESP32CamError(full.strip())
            raise ESP32CamError(f"Cannot parse SIZE from: {full.strip()}")

        file_size = int(match.group(1))
        self._log(f"  File size: {file_size} bytes")

        # Send ACK
        self._log("  Sending ACK...")
        self.ser.write(b"ACK\n")
        self.ser.flush()

        result = bytearray()
        remaining = file_size
        chunk_num = 0

        while remaining > 0:
            chunk_num += 1
            chunk_len = min(CHUNK_SIZE, remaining)

            # Read CHECKSUM line — search for prefix, skip garbage
            checksum_line = self._read_line(timeout=10, expected_prefix="CHECKSUM:")
            if not checksum_line.startswith("CHECKSUM:"):
                # Try once more after a short delay (stale data may still be arriving)
                time.sleep(0.2)
                checksum_line = self._read_line(timeout=5, expected_prefix="CHECKSUM:")
                if not checksum_line.startswith("CHECKSUM:"):
                    raise ESP32CamError(f"Expected CHECKSUM, got: {checksum_line!r}")
            expected_checksum = int(checksum_line.split(":", 1)[1].strip())
            self._log(f"  Chunk {chunk_num}: expecting checksum {expected_checksum}")

            # Send ACKCHECKSUM
            self.ser.write(b"ACKCHECKSUM\n")
            self.ser.flush()

            # Read chunk data — drain exactly chunk_len bytes, retrying on short reads
            try:
                chunk = self._drain_bytes(chunk_len, timeout=5.0)
            except ESP32CamError as e:
                self._log(f"  Chunk {chunk_num}: {e}")
                self.ser.write(b"NACKDATA\n")
                self.ser.flush()
                continue  # Chunk will be resent

            # Verify checksum
            actual_checksum = compute_checksum(chunk)
            if actual_checksum != expected_checksum:
                self._log(
                    f"  Chunk {chunk_num}: checksum mismatch! "
                    f"expected={expected_checksum}, got={actual_checksum}"
                )
                self.ser.write(b"NACKDATA\n")
                self.ser.flush()
                continue  # Chunk will be resent

            result.extend(chunk)
            remaining -= chunk_len
            self._log(f"  Chunk {chunk_num}: OK ({chunk_len} b), remaining={remaining}")

            # Send ACKDATA
            self.ser.write(b"ACKDATA\n")
            self.ser.flush()

        self._log(f"  All {file_size} bytes received")

        # Consume EOT and trailing prompt
        tail = b""
        start = time.time()
        while time.time() - start < 5:
            if self.ser.in_waiting:
                tail += self.ser.read(self.ser.in_waiting)
                if b"\x04" in tail or b"> " in tail:
                    has_eot = b"\x04" in tail
                    self._log(f"  Tail: EOT={has_eot}, len={len(tail)}")
                    break
            time.sleep(0.05)

        return bytes(result)

    def delete_file(self, path: str) -> bool:
        """Delete a file on the device."""
        self._log(f"delete_file('{path}')")
        response = self._send_cmd(f"DELETE {path}")
        self._log(f"  Response: {response[:100]!r}")
        if "[OK]" in response:
            self._log("  Delete succeeded")
            return True
        if "[ERROR]" in response:
            self._log("  Delete failed")
            raise ESP32CamError(response.strip())
        self._log("  Unexpected response, treating as failure")
        return False

    def store_file(self, path: str, data: bytes) -> bool:
        """Upload a file to the device using chunked checksum protocol."""
        size = len(data)
        self._log(f"store_file('{path}', {size} bytes)")
        response = self._send_cmd(f"STORE {path} {size}")
        self._log(f"  Pre-upload response: {response[:150]!r}")

        if "[OK] Ready to receive" not in response and "[OK] Ready" not in response:
            if "[ERROR]" in response:
                self._log("  Device rejected STORE")
                raise ESP32CamError(response.strip())
            self._log("  Unexpected STORE response")
            raise ESP32CamError(f"Unexpected STORE response: {response}")

        self._log("  Sending ACK...")
        self.ser.write(b"ACK\n")
        self.ser.flush()

        offset = 0
        chunk_num = 0
        while offset < size:
            chunk_num += 1
            chunk = data[offset:offset + CHUNK_SIZE]
            chunk_len = len(chunk)
            checksum = compute_checksum(chunk)

            # Retry loop for this chunk
            for attempt in range(3):
                self._log(
                    f"  Chunk {chunk_num}: attempt {attempt + 1}, "
                    f"{chunk_len} bytes, checksum={checksum}"
                )

                # Send CHECKSUM
                self.ser.write(f"CHECKSUM: {checksum}\n".encode())
                self.ser.flush()

                # Read ACKCHECKSUM
                ack = self._read_line(timeout=5, expected_prefix="ACKCHECKSUM")
                if ack != "ACKCHECKSUM":
                    self._log(f"    Expected ACKCHECKSUM, got: {ack!r}")
                    time.sleep(0.2)
                    continue

                # Send chunk data
                self.ser.write(chunk)
                self.ser.flush()

                # Read ACKDATA or NACKDATA
                ack = self._read_line(timeout=5)
                if ack == "NACKDATA":
                    self._log(f"    NACKDATA received, retrying")
                    continue
                if ack != "ACKDATA":
                    self._log(f"    Expected ACKDATA, got: {ack!r}")
                    time.sleep(0.2)
                    continue

                # Success
                offset += chunk_len
                self._log(f"    ACKDATA, offset={offset}/{size}")
                break
            else:
                # All 3 attempts failed
                raise ESP32CamError(f"Chunk {chunk_num} failed after 3 attempts")

        # Read final response
        response = self._read_until_prompt()
        self._log(f"  Post-upload response: {response[:150]!r}")
        if "[OK]" in response:
            self._log("  Upload succeeded")
            return True
        if "[ERROR]" in response:
            self._log("  Upload failed")
            raise ESP32CamError(response.strip())
        self._log("  Unexpected post-upload response")
        return False

    def switch_mode(self, mode: str) -> bool:
        """Switch the device mode (``photo`` or ``transfer``)."""
        mode_u = mode.upper()
        self._log(f"switch_mode('{mode_u}')")
        response = self._send_cmd(f"MODE {mode_u}")
        self._log(f"  Response: {response[:100]!r}")
        if "[OK]" in response:
            self._log("  Mode switch acknowledged")
            if mode_u == "PHOTO":
                self._log("  Device entering deep sleep, disconnecting")
                self.disconnect()
            return True
        if "[ERROR]" in response:
            self._log("  Mode switch failed")
            raise ESP32CamError(response.strip())
        self._log("  Unexpected mode switch response")
        return False

    # ---------- REPL ----------

    def repl(self) -> None:
        """Interactive pass-through REPL."""
        print(f"Connected to {self.port} at {self.baud} baud.")
        print("Type '!quit' to exit.  Raw text is sent to the device.")
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
                response = self._send_cmd(user_input)
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
