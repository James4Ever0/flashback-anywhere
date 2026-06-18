#!/usr/bin/env python3
"""
Minimal Bluetooth Serial FTP Client for ESP32-CAM

Usage:
    python bt_ftp_client.py /dev/rfcomm0
    python bt_ftp_client.py /dev/pts/7        # PTY mock testing
    python bt_ftp_client.py COM5              # Windows

Dependencies:
    pip install pyserial>=3.5

The port can be any pyserial-compatible path:
    - Real Bluetooth RFCOMM: /dev/rfcomm0 (Linux), COMx (Windows)
    - PTY mock: /dev/pts/N (Linux, for testing without hardware)
    - macOS paired: /dev/tty.ESP32-FTP

Baud rate is passed to pyserial for API compatibility but is ignored
by Bluetooth SPP virtual serial ports. Use any value (default 115200).
"""

import sys
import serial
import os
import time


CHUNK_SIZE = 1024
EOT = b'\x04'
PROMPT = b'> '

# ---------------------------------------------------------------------------
# Verbose logging helpers
# ---------------------------------------------------------------------------
VERBOSE = True  # Set to False to silence all debug output


def vprint(msg: str) -> None:
    """Print a verbose log line with timestamp."""
    if VERBOSE:
        ts = time.strftime("%H:%M:%S", time.localtime())
        ms = int((time.time() % 1) * 1000)
        print(f"[{ts}.{ms:03d}] {msg}")


def vhex(label: str, data: bytes, max_len: int = 64) -> None:
    """Print hex dump of bytes (truncated if longer than max_len)."""
    if not VERBOSE:
        return
    preview = data[:max_len]
    hex_str = preview.hex(' ')
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in preview)
    truncated = " ..." if len(data) > max_len else ""
    vprint(f"{label}: len={len(data)} hex={hex_str}{truncated} ascii={ascii_str!r}{truncated}")


# ---------------------------------------------------------------------------
class BtFTPError(Exception):
    pass


class BtFTPClient:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 5.0):
        """
        timeout: per-read timeout in seconds. 5s is plenty for Bluetooth;
        30s just masks stalls.
        """
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser: serial.Serial | None = None

    def connect(self) -> None:
        """Open the serial port and verify the ESP32 responds."""
        vprint("=" * 60)
        vprint("CONNECT starting")
        vprint(f"  port={self.port}")
        vprint(f"  baudrate={self.baud}")
        vprint(f"  timeout={self.timeout}s")

        try:
            t0 = time.time()
            self.ser = serial.Serial(
                self.port,
                baudrate=self.baud,
                timeout=self.timeout,
                write_timeout=self.timeout,
            )
            t1 = time.time()
            vprint(f"Serial port opened in {(t1 - t0) * 1000:.1f}ms")
            vprint(f"  fd={self.ser.fd}")
            vprint(f"  in_waiting={self.ser.in_waiting}")
        except serial.SerialException as e:
            raise BtFTPError(f"Cannot open port {self.port}: {e}")

        # Flush any stale bytes
        stale = self.ser.in_waiting
        if stale > 0:
            stale_data = self.ser.read(stale)
            vhex("Stale bytes discarded", stale_data, 256)

        # Handshake: send HELP, expect prompt
        vprint("Handshake: HELP")
        self._send_raw(b"HELP\n")
        t0 = time.time()
        response = self._read_until_prompt()
        t1 = time.time()
        vprint(f"Handshake response in {(t1 - t0) * 1000:.1f}ms")
        vhex("Handshake raw", response, 512)

        if b"Available commands" not in response and b">" not in response:
            decoded = response.decode(errors='replace')
            raise BtFTPError(f"Bad handshake:\n{decoded}")

        print(f"[OK] Connected to {self.port}")
        vprint("=" * 60)

    def disconnect(self) -> None:
        if self.ser:
            self.ser.close()
            self.ser = None

    def _send_raw(self, data: bytes) -> None:
        if not self.ser:
            raise BtFTPError("Not connected")
        t0 = time.time()
        written = self.ser.write(data)
        self.ser.flush()
        t1 = time.time()
        vprint(f"TX {len(data)} bytes, written={written}, flush={(t1-t0)*1000:.1f}ms")
        vhex("  TX", data, 64)

    def _read_until_prompt(self) -> bytes:
        """Read the full response, then find the LAST prompt.

        Uses self.ser.in_waiting to poll without blocking, so we can
        detect end-of-stream quickly instead of waiting for the full
        serial timeout on every empty read.
        """
        if not self.ser:
            raise BtFTPError("Not connected")

        buf = b""
        empty_rounds = 0
        t0 = time.time()

        poll_round_limit = 50

        while True:
            avail = self.ser.in_waiting
            if avail > 0:
                chunk = self.ser.read(min(4096, avail))
                buf += chunk
                empty_rounds = 0
                vprint(f"  RX: {len(chunk)} bytes (total buf={len(buf)})")
                if chunk.endswith(b"> "):
                    vprint("Prompt found at current chunk, reducing poll round limit to 10.")
                    poll_round_limit = 10
                else:
                    if poll_round_limit != 50:
                        vprint("Revoking poll round limit to 50 since previous prompt is not final.")
                        poll_round_limit = 50
                if len(chunk) <= 64:
                    vhex("    hex", chunk)
            else:
                empty_rounds += 1
                # 50 empty polls = ~500ms of silence → response is done
                if empty_rounds >= poll_round_limit:
                    vprint("  Stream idle ~%sms — response complete" % (poll_round_limit * 10))
                    break
                elapsed = time.time() - t0
                if elapsed > self.timeout:
                    vprint(f"  TIMEOUT after {elapsed:.1f}s, buf={len(buf)}")
                    break
                time.sleep(0.01)

        # Find the LAST prompt — that's the real one.
        last_pos = buf.rfind(PROMPT)
        if last_pos >= 0:
            buf = buf[: last_pos + len(PROMPT)]
            vprint(f"  Last prompt at byte {last_pos}")
        else:
            vprint("  WARNING: no prompt found")

        vprint(f"  Total: {len(buf)} bytes in {(time.time() - t0) * 1000:.1f}ms")
        return buf

    def _drain_stale(self) -> bytes:
        """Non-blocking drain of any stale data in the OS serial buffer.
        Returns drained bytes. Prints a warning if anything was found.
        """
        if not self.ser:
            return b""
        drained = b""
        # Poll for up to 200ms, reading whatever is available
        for _ in range(20):
            avail = self.ser.in_waiting
            if avail > 0:
                chunk = self.ser.read(min(4096, avail))
                drained += chunk
            elif drained:
                # No more data and we already drained something — stop
                break
            time.sleep(0.01)

        if drained:
            decoded = drained.decode(errors="replace").rstrip()
            print(f"[WARN] Unparsed stale data found BEFORE command ({len(drained)} bytes):")
            for line in decoded.splitlines():
                print(f"       | {line}")
        return drained

    def _cmd(self, cmd: str) -> str:
        """Send a command and return the text response (without prompt)."""
        vprint(f"CMD: {cmd!r}")

        # Drain any leftover data from previous commands before sending
        stale = self._drain_stale()
        if stale:
            vhex("  Stale drained", stale, 256)

        self._send_raw((cmd + "\n").encode())
        raw = self._read_until_prompt()
        text = raw.decode(errors="replace")
        if text.rstrip().endswith(">"):
            text = text.rstrip()[:-1].rstrip()
        vprint(f"Response ({len(text)} chars):")
        for line in text.splitlines():
            vprint(f"  | {line}")
        return text

    # ------------------------------------------------------------------
    # High-level API
    # ------------------------------------------------------------------

    def help(self) -> str:
        return self._cmd("HELP")

    def list_files(self, path: str = "/") -> list[dict]:
        text = self._cmd(f"LIST {path}")
        items = []
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("[DIR]"):
                name = line[5:].strip()
                items.append({"type": "dir", "name": name})
            elif line.startswith("[FILE]"):
                rest = line[6:].strip()
                parts = rest.split(None, 2)
                if len(parts) >= 2:
                    size = int(parts[0])
                    name = parts[2] if len(parts) > 2 else parts[1]
                    items.append({"type": "file", "name": name, "size": size})
        return items

    def read_file(self, path: str) -> bytes:
        """Download a file. Returns raw bytes."""
        self._send_raw(f"READ {path}\n".encode())
        header = b""
        while b"\n" not in header:
            chunk = self.ser.read(1)
            if not chunk:
                raise BtFTPError("Timeout waiting for READ header")
            header += chunk

        header_str = header.decode(errors="replace")
        if not header_str.startswith("SIZE:"):
            rest = self._read_until_prompt()
            raise BtFTPError((header_str + rest.decode(errors="replace")).strip())

        file_size = int(header_str.replace("SIZE:", "").strip())
        vprint(f"File size: {file_size}")

        data = b""
        t0 = time.time()
        while len(data) < file_size:
            to_read = min(CHUNK_SIZE, file_size - len(data))
            chunk = self.ser.read(to_read)
            if not chunk:
                raise BtFTPError(f"Timeout during read. Got {len(data)}/{file_size}")
            data += chunk
        
        t1 = time.time()
        vprint("Transfer file taking %s seconds" % (t1-t0))

        # Consume EOT
        eot = self.ser.read(1)
        if eot != EOT:
            if not data.endswith(EOT):
                raise BtFTPError("Missing EOT")
            data = data[:-1]

        self._read_until_prompt()  # drain trailing prompt
        return data

    def _read_line(self, timeout: float = 5.0) -> bytes:
        """Read bytes until '\n' (single line)."""
        buf = b""
        t0 = time.time()
        while b"\n" not in buf:
            if self.ser.in_waiting > 0:
                buf += self.ser.read(1)
            elif time.time() - t0 > timeout:
                break
            else:
                time.sleep(0.005)
        return buf

    def store_file(self, path: str, data: bytes) -> None:
        size = len(data)
        vprint(f"STORE {path} ({size} bytes)")

        # Drain stale data
        stale = self._drain_stale()
        if stale:
            vhex("  Stale drained", stale, 256)

        # Send STORE command
        self._send_raw(f"STORE {path} {size}\n".encode())

        # Read response line: should be "[OK] Ready to receive" or "[ERROR] ..."
        vprint("Waiting for 'Ready to receive'...")
        line = self._read_line(timeout=5.0)
        line_str = line.decode(errors="replace").strip()
        vprint(f"  Store init response: {line_str!r}")

        if "[OK] Ready to receive" not in line_str:
            # Might be an error — read rest and raise
            rest = self._read_until_prompt()
            raise BtFTPError(line_str + "\n" + rest.decode(errors="replace"))

        # ------------------------------------------------------------------
        # Stop-and-wait upload: send one chunk, wait for ACK, then next.
        # This prevents the ESP32 Bluetooth RX buffer from overflowing.
        # ------------------------------------------------------------------
        TX_CHUNK = 512          # must be <= board's RX_CHUNK (512)
        MAX_RETRIES = 3
        pos = 0                 # offset into *data* that the board has confirmed
        retries = 0
        t0 = time.time()

        while pos < size:
            end = min(pos + TX_CHUNK, size)
            chunk = data[pos:end]

            self._send_raw(chunk)
            vprint(f"  Sent chunk  pos={pos}  len={len(chunk)}  "
                   f"({(pos * 100) // size}%)")

            # Wait for ACK: "[ACK] <cumulative_bytes>\n"
            ack_line = self._read_line(timeout=10.0)
            ack_str = ack_line.decode(errors="replace").strip()
            vprint(f"  ACK raw: {ack_str!r}")

            if not ack_str.startswith("[ACK]"):
                raise BtFTPError(f"Expected [ACK], got: {ack_str}")

            try:
                ack_total = int(ack_str.split()[1])
            except (IndexError, ValueError) as e:
                raise BtFTPError(f"Invalid ACK: {ack_str} ({e})")

            expected = end

            if ack_total > expected:
                raise BtFTPError(
                    f"ACK {ack_total} > expected {expected}"
                )

            if ack_total == pos:
                # Board made zero progress — likely BT buffer dropped everything.
                retries += 1
                if retries > MAX_RETRIES:
                    raise BtFTPError(
                        f"No progress after {MAX_RETRIES} retries "
                        f"(stuck at {pos}/{size})"
                    )
                vprint(f"  No progress, retrying ({retries}/{MAX_RETRIES})...")
                time.sleep(0.3)
                continue

            # Progress made (full or partial chunk)
            if ack_total < expected:
                vprint(
                    f"  Partial ACK: {ack_total}/{expected}. "
                    f"Resuming from offset {ack_total}"
                )

            pos = ack_total
            retries = 0

        t1 = time.time()
        vprint(f"All {size} bytes sent+ACK'd in {(t1 - t0) * 1000:.1f}ms")

        # Wait for final confirmation + prompt
        vprint("Waiting for store confirmation...")
        confirm = self._read_until_prompt()
        confirm_str = confirm.decode(errors="replace")
        vprint(f"Confirmation: {confirm_str!r}")
        if "[OK]" not in confirm_str:
            raise BtFTPError(confirm_str)

    def delete(self, path: str) -> None:
        response = self._cmd(f"DELETE {path}")
        if "[OK]" not in response:
            raise BtFTPError(response)

    def auth(self, pin: str) -> None:
        response = self._cmd(f"AUTH {pin}")
        if "[OK]" not in response:
            raise BtFTPError(response)

    def bt_info(self) -> str:
        return self._cmd("BTINFO")

    # ------------------------------------------------------------------
    # Interactive REPL
    # ------------------------------------------------------------------

    def repl(self) -> None:
        print("Entering REPL. Type !quit to exit.")
        print("Special: !auth, !list, !download, !upload, !delete, !info")
        while True:
            try:
                user = input("esp32-bt> ").strip()
            except (EOFError, KeyboardInterrupt):
                break

            if not user:
                continue
            if user in ("!quit", "!exit"):
                break

            if user.startswith("!auth"):
                parts = user.split(None, 1)
                if len(parts) != 2:
                    print("Usage: !auth <pin>")
                    continue
                try:
                    self.auth(parts[1])
                    print("[OK] Authenticated")
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            if user.startswith("!list"):
                path = user[5:].strip() or "/"
                try:
                    for item in self.list_files(path):
                        t = item["type"]
                        s = f"{item['size']:>8} bytes" if t == "file" else "           "
                        print(f"  [{t:4}] {s}  {item['name']}")
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            if user.startswith("!download"):
                parts = user.split(None, 2)
                if len(parts) != 3:
                    print("Usage: !download <remote> <local>")
                    continue
                try:
                    data = self.read_file(parts[1])
                    with open(parts[2], "wb") as f:
                        f.write(data)
                    print(f"[OK] Downloaded {len(data)} bytes")
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            if user.startswith("!upload"):
                parts = user.split(None, 2)
                if len(parts) != 3:
                    print("Usage: !upload <local> <remote>")
                    continue
                if not os.path.exists(parts[1]):
                    print(f"[ERROR] File not found: {parts[1]}")
                    continue
                try:
                    with open(parts[1], "rb") as f:
                        self.store_file(parts[2], f.read())
                    print("[OK] Uploaded")
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            if user.startswith("!delete"):
                parts = user.split(None, 1)
                if len(parts) != 2:
                    print("Usage: !delete <remote>")
                    continue
                try:
                    self.delete(parts[1])
                    print("[OK] Deleted")
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            if user.startswith("!info"):
                try:
                    print(self.bt_info())
                except BtFTPError as e:
                    print(f"[ERROR] {e}")
                continue

            try:
                print(self._cmd(user))
            except BtFTPError as e:
                print(f"[ERROR] {e}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <port> [baud]")
        sys.exit(1)

    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    client = BtFTPClient(port, baud=baud, timeout=5.0)
    try:
        client.connect()
        client.repl()
    except BtFTPError as e:
        print(f"[ERROR] {e}")
        sys.exit(1)
    finally:
        client.disconnect()
        print("[OK] Disconnected")


if __name__ == "__main__":
    main()
