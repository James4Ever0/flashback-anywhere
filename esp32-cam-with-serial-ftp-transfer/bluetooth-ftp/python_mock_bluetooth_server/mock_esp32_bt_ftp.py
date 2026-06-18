#!/usr/bin/env python3
"""
Mock ESP32 Bluetooth FTP Server

Simulates the ESP32 BluetoothFTPServer.ino behavior over a PTY (pseudo-terminal).
Use this to test the bt_ftp_client.py without real hardware.

Usage:
    python mock_esp32_bt_ftp.py [virtual_sd_directory]

The server prints a PTY slave path like /dev/pts/7.
Point the client at that path:
    python bt_ftp_client.py /dev/pts/7

Protocol (same as ESP32):
    - Commands end with \n
    - Responses end with \n, final line is "> " prompt
    - READ: "SIZE: N\n" + binary data + \x04 (EOT)
    - STORE: "[OK] Ready to receive", then client sends raw bytes
"""

import os
import sys
import pty
import select
import struct


# Use a local directory as the "SD card"
VSD_DIR = sys.argv[1] if len(sys.argv) > 1 else "./virtual_sd"
EOT = b'\x04'
PROMPT = b'> '

# Mock auth mode (set to True to test AUTH command)
AUTH_REQUIRED = False
is_authenticated = False
expected_pin = ""


def ensure_vsd():
    os.makedirs(VSD_DIR, exist_ok=True)


def list_dir(path: str) -> bytes:
    target = os.path.join(VSD_DIR, path.lstrip("/"))
    if not os.path.isdir(target):
        return f"[ERROR] Failed to open directory: {path}\n".encode()

    lines = b""
    try:
        for entry in sorted(os.listdir(target)):
            full = os.path.join(target, entry)
            if os.path.isdir(full):
                lines += f"[DIR]  {entry}\n".encode()
            else:
                sz = os.path.getsize(full)
                lines += f"[FILE]  {sz:>8} bytes  {entry}\n".encode()
    except OSError as e:
        return f"[ERROR] {e}\n".encode()
    return lines


def read_file(path: str) -> bytes:
    target = os.path.join(VSD_DIR, path.lstrip("/"))
    if not os.path.isfile(target):
        return f"[ERROR] Failed to open file: {path}\n".encode()

    try:
        with open(target, "rb") as f:
            data = f.read()
    except OSError as e:
        return f"[ERROR] {e}\n".encode()

    header = f"SIZE: {len(data)}\n".encode()
    return header + data + EOT


def store_file(path: str, size: int, master_fd: int) -> bytes:
    target = os.path.join(VSD_DIR, path.lstrip("/"))
    os.makedirs(os.path.dirname(target) or ".", exist_ok=True)

    # Tell client we are ready
    os.write(master_fd, b"[OK] Ready to receive\n")

    # Read exactly `size` bytes from the PTY
    data = b""
    while len(data) < size:
        remaining = size - len(data)
        chunk = os.read(master_fd, min(remaining, 4096))
        if not chunk:
            break
        data += chunk

    try:
        with open(target, "wb") as f:
            f.write(data)
        return f"[OK] File stored: {path} ({len(data)} bytes)\n".encode()
    except OSError as e:
        return f"[ERROR] Failed to write file: {e}\n".encode()


def auth_check(cmd: str) -> bytes | None:
    """Return error bytes if auth is required and not authenticated."""
    if not AUTH_REQUIRED:
        return None
    if cmd in ("AUTH", "HELP", "BTINFO"):
        return None
    if not is_authenticated:
        return b"[ERROR] Not authenticated. Send AUTH <pin> first.\n"
    return None


def delete_file(path: str) -> bytes:
    target = os.path.join(VSD_DIR, path.lstrip("/"))
    try:
        os.remove(target)
        return f"[OK] Deleted: {path}\n".encode()
    except FileNotFoundError:
        return f"[ERROR] Failed to delete: {path}\n".encode()
    except OSError as e:
        return f"[ERROR] {e}\n".encode()


def handle_command(line: str, master_fd: int) -> bytes:
    line = line.strip()
    if not line:
        return b""

    parts = line.split(None, 2)
    cmd = parts[0].upper() if parts else ""

    # Auth gate
    auth_err = auth_check(cmd)
    if auth_err:
        return auth_err

    if cmd == "HELP":
        help_text = b"Available commands:\n"
        if AUTH_REQUIRED:
            help_text += b"  AUTH   <pin>               - Authenticate before using FTP\n"
        help_text += (
            b"  LIST   <path>              - List directory contents\n"
            b"  READ   <path>              - Read file and send binary\n"
            b"  STORE  <path> <size>       - Write file from binary data\n"
            b"  DELETE <path>              - Delete a file\n"
            b"  SETPIN <pin>               - Set new PIN (reboot to apply)\n"
            b"  BTINFO                     - Show Bluetooth info\n"
            b"  HELP                       - Show this message\n"
        )
        return help_text

    elif cmd == "AUTH":
        global is_authenticated, expected_pin
        if len(parts) < 2:
            return b"[ERROR] Usage: AUTH <pin>\n"
        pin = parts[1]
        pinfile = os.path.join(VSD_DIR, "btpin.txt")
        if os.path.exists(pinfile):
            with open(pinfile) as f:
                expected_pin = f.read().strip()
        if expected_pin and pin == expected_pin:
            is_authenticated = True
            return b"[OK] Authenticated.\n"
        else:
            return b"[ERROR] Invalid PIN.\n"

    elif cmd == "LIST":
        path = parts[1] if len(parts) > 1 else "/"
        return list_dir(path)

    elif cmd == "READ":
        if len(parts) < 2:
            return b"[ERROR] Usage: READ <path>\n"
        return read_file(parts[1])

    elif cmd == "STORE":
        if len(parts) < 3:
            return b"[ERROR] Usage: STORE <path> <size>\n"
        try:
            size = int(parts[2])
        except ValueError:
            return b"[ERROR] Usage: STORE <path> <size>\n"
        return store_file(parts[1], size, master_fd)

    elif cmd == "DELETE":
        if len(parts) < 2:
            return b"[ERROR] Usage: DELETE <path>\n"
        return delete_file(parts[1])

    elif cmd == "SETPIN":
        if len(parts) < 2:
            return b"[ERROR] Usage: SETPIN <pin>\n"
        pin = parts[1]
        pinfile = os.path.join(VSD_DIR, "btpin.txt")
        with open(pinfile, "w") as f:
            f.write(pin + "\n")
        return f"[OK] PIN saved as '{pin}'. Reboot to apply.\n".encode()

    elif cmd == "BTINFO":
        pin = "0000"
        pinfile = os.path.join(VSD_DIR, "btpin.txt")
        if os.path.exists(pinfile):
            with open(pinfile) as f:
                pin = f.read().strip() or "0000"
        auth_status = "yes" if is_authenticated else "no"
        return (
            f"Device:        ESP32-FTP\n"
            f"App PIN:       {pin}\n"
            f"Client:        connected\n"
            f"Authenticated: {auth_status}\n"
            f"(MAC: mock AA:BB:CC:DD:EE:FF)\n"
        ).encode()

    else:
        return f"[ERROR] Unknown command: {cmd}\n".encode()


def main():
    ensure_vsd()

    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)

    print(f"=" * 60)
    print(f"Mock ESP32 Bluetooth FTP Server")
    print(f"Virtual SD directory: {os.path.abspath(VSD_DIR)}")
    print(f"PTY slave path: {slave_name}")
    print(f"")
    print(f"Connect client with:")
    print(f"  python bt_ftp_client.py {slave_name}")
    print(f"=" * 60)
    print(f"Waiting for client connection...\n")

    buf = b""

    # Initial prompt
    os.write(master_fd, PROMPT)

    try:
        while True:
            # Wait for data from the PTY master
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if master_fd in ready:
                data = os.read(master_fd, 1024)
                if not data:
                    break
                buf += data

                # Process complete lines
                while b'\n' in buf:
                    line, _, buf = buf.partition(b'\n')
                    line = line.replace(b'\r', b'')

                    response = handle_command(line.decode(errors="replace"), master_fd)
                    if response:
                        os.write(master_fd, response)
                    os.write(master_fd, PROMPT)

    except KeyboardInterrupt:
        print("\n[OK] Server shutting down.")
    finally:
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    main()
