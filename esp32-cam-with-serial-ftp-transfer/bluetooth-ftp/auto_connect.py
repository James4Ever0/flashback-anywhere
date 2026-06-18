#!/usr/bin/env python3
"""
Auto-connect script for ESP32-CAM Bluetooth FTP server.

Scans for "ESP32-FTP", pairs if needed (handling SSP confirmation),
trusts, binds RFCOMM port 0, then launches the FTP client.

Dependencies:
    pip install pexpect pyserial

Usage:
    python3 auto_connect.py
    python3 auto_connect.py --client    # also launch bt_ftp_client.py after connect
    python3 auto_connect.py --mac F0:24:F9:F7:35:42  # skip scan, use known MAC
"""

import argparse
import os
import re
import subprocess
import sys
import time

PEXPECT_AVAILABLE = False
try:
    import pexpect
    PEXPECT_AVAILABLE = True
except ImportError:
    pass

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
DEVICE_NAME = "ESP32-FTP"
RFCOMM_PORT = 0
RFCOMM_DEV = f"/dev/rfcomm{RFCOMM_PORT}"
SCAN_TIMEOUT = 30       # seconds to wait for device during scan
PAIR_TIMEOUT = 30       # seconds for pairing process
BIND_TIMEOUT = 10       # seconds for rfcomm bind

# ---------------------------------------------------------------------------
def run_cmd(cmd: list[str], timeout: int = 10) -> tuple[int, str, str]:
    """Run a shell command, return (rc, stdout, stderr)."""
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError as e:
        return -1, "", str(e)


def rfcomm_release(port: int = RFCOMM_PORT) -> bool:
    """Release a stale RFCOMM binding."""
    rc, out, err = run_cmd(["sudo", "rfcomm", "release", str(port)], timeout=5)
    if rc == 0:
        print(f"[OK] rfcomm release {port}")
        return True
    # Already released or not found is fine
    if "Can" in err or "can" in err.lower() or "not" in err.lower():
        print(f"[INFO] rfcomm release {port}: nothing to release (OK)")
        return True
    print(f"[WARN] rfcomm release {port}: {err.strip() or out.strip()}")
    return True


def rfcomm_bind(mac: str, port: int = RFCOMM_PORT) -> bool:
    """Bind an RFCOMM virtual serial port."""
    print(f"[INFO] Binding rfcomm {port} to {mac} ...")
    rc, out, err = run_cmd(["sudo", "rfcomm", "bind", str(port), mac], timeout=BIND_TIMEOUT)
    if rc != 0:
        print(f"[ERROR] rfcomm bind failed: {err.strip() or out.strip()}")
        return False
    # Verify device exists
    if os.path.exists(RFCOMM_DEV):
        print(f"[OK] {RFCOMM_DEV} created")
        return True
    print(f"[ERROR] {RFCOMM_DEV} did not appear after bind")
    return False


def bluetoothctl_find_mac(name: str, timeout: int = SCAN_TIMEOUT) -> str | None:
    """
    Scan for a Bluetooth device by name and return its MAC.
    Uses pexpect to drive bluetoothctl interactively.
    """
    if not PEXPECT_AVAILABLE:
        print("[ERROR] pexpect is required. Install: pip install pexpect")
        sys.exit(1)

    print(f"[INFO] Scanning for '{name}' (up to {timeout}s) ...")
    try:
        child = pexpect.spawn("bluetoothctl", encoding="utf-8", timeout=timeout)
    except Exception as e:
        print(f"[ERROR] Failed to start bluetoothctl: {e}")
        return None

    mac = None
    try:
        child.sendline("power on")
        child.expect(r"#", timeout=5)

        child.sendline("scan on")
        # Wait until we see the device appear
        pattern = re.compile(rf"\[NEW\] Device ([0-9A-F:]{{17}}) {re.escape(name)}")
        alt_pattern = re.compile(rf"Device ([0-9A-F:]{{17}}) {re.escape(name)}")

        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                child.expect([pattern, alt_pattern, r"#"], timeout=2)
                match = child.match
                if match and match.lastindex and len(match.groups()) >= 1:
                    mac = match.group(1)
                    print(f"[OK] Found {name} at {mac}")
                    break
            except pexpect.TIMEOUT:
                continue
            except pexpect.EOF:
                break

        child.sendline("scan off")
        child.expect(r"#", timeout=5)
        child.sendline("quit")
    except pexpect.TIMEOUT:
        print(f"[ERROR] Scan timeout — {name} not found within {timeout}s")
    except Exception as e:
        print(f"[ERROR] Scan error: {e}")
    finally:
        try:
            child.close()
        except Exception:
            pass

    return mac


def bluetoothctl_pair_and_trust(mac: str, timeout: int = PAIR_TIMEOUT) -> bool:
    """
    Pair and trust a device using bluetoothctl + pexpect.
    Handles the SSP numeric-comparison prompt in the terminal (no GUI).
    """
    if not PEXPECT_AVAILABLE:
        print("[ERROR] pexpect is required. Install: pip install pexpect")
        return False

    print(f"[INFO] Pairing {mac} ...")
    try:
        child = pexpect.spawn("bluetoothctl", encoding="utf-8", timeout=timeout)
    except Exception as e:
        print(f"[ERROR] Failed to start bluetoothctl: {e}")
        return False

    try:
        child.expect(r"#", timeout=5)

        # Register agent so prompts appear in terminal, not GUI
        child.sendline("agent on")
        child.expect(r"Agent registered", timeout=5)
        child.sendline("default-agent")
        child.expect(r"Default agent request successful", timeout=5)

        # Check if already paired
        child.sendline(f"info {mac}")
        idx = child.expect([r"Paired: yes", r"Paired: no", r"Device {mac} not available".format(mac=mac), r"#"], timeout=5)
        already_paired = (idx == 0)

        if already_paired:
            print("[INFO] Device already paired")
        else:
            # Pair
            child.sendline(f"pair {mac}")
            # Possible responses during pairing
            pair_patterns = [
                r"Confirm passkey \d+ \(yes\/no\):",   # 0
                r"Pairing successful",                    # 1
                r"Failed to pair",                        # 2
                r"Already Exists",                        # 3
                r"AuthenticationFailed",                  # 4
                r"Connected: yes",                        # 5
                r"#",                                     # 6
            ]
            while True:
                idx = child.expect(pair_patterns, timeout=timeout)
                if idx == 0:
                    # SSP numeric comparison — auto-confirm
                    print("[INFO] SSP numeric comparison detected — confirming")
                    child.sendline("yes")
                elif idx == 1:
                    print("[OK] Pairing successful")
                    break
                elif idx == 2:
                    print("[ERROR] Failed to pair")
                    return False
                elif idx == 3:
                    print("[OK] Already paired")
                    break
                elif idx == 4:
                    print("[ERROR] Authentication failed — remove old bond and retry")
                    return False
                elif idx == 5:
                    print("[INFO] Connected during pairing")
                elif idx == 6:
                    # Got prompt back — check result
                    break

        # Trust
        child.sendline(f"trust {mac}")
        trust_idx = child.expect([r"trust succeeded", r"Already Exists", r"#"], timeout=10)
        if trust_idx in (0, 1):
            print("[OK] Device trusted")
        else:
            print("[INFO] Trust command completed")

        child.sendline("quit")
        child.expect(pexpect.EOF, timeout=5)
        return True

    except pexpect.TIMEOUT:
        print("[ERROR] bluetoothctl timeout during pairing")
        return False
    except Exception as e:
        print(f"[ERROR] Pairing exception: {e}")
        return False
    finally:
        try:
            child.close()
        except Exception:
            pass


def remove_bond(mac: str) -> bool:
    """Remove a stale bond from the PC side."""
    rc, out, err = run_cmd(["bluetoothctl", "remove", mac], timeout=10)
    if rc == 0 or "not available" in (out + err).lower():
        print(f"[OK] Bond removed (or did not exist)")
        return True
    print(f"[WARN] Remove bond: {err.strip() or out.strip()}")
    return True


def is_rfcomm_bound(port: int = RFCOMM_PORT) -> bool:
    """Check if an RFCOMM device is currently bound."""
    return os.path.exists(f"/dev/rfcomm{port}")


def is_device_connected(mac: str) -> bool:
    """Quick check via bluetoothctl info."""
    rc, out, err = run_cmd(["bluetoothctl", "info", mac], timeout=5)
    return "Connected: yes" in out


def main():
    parser = argparse.ArgumentParser(description="Auto-connect to ESP32-FTP Bluetooth server")
    parser.add_argument("--mac", help="Skip scan, use known MAC address")
    parser.add_argument("--client", action="store_true", help="Launch bt_ftp_client.py after connecting")
    parser.add_argument("--re-pair", action="store_true", help="Remove old bond and re-pair")
    args = parser.parse_args()

    # --- Step 1: Find MAC ---
    mac = args.mac
    if not mac:
        mac = bluetoothctl_find_mac(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if not mac:
            print(f"[ERROR] Could not find '{DEVICE_NAME}'. Is it powered on and in range?")
            sys.exit(1)

    print(f"[INFO] Target MAC: {mac}")

    # --- Step 2: Re-pair if requested ---
    if args.re_pair:
        print("[INFO] Re-pair requested: removing old bond")
        remove_bond(mac)

    # --- Step 3: Pair + Trust ---
    if not bluetoothctl_pair_and_trust(mac, timeout=PAIR_TIMEOUT):
        print("[ERROR] Pair/trust failed")
        sys.exit(1)

    # --- Step 4: Release stale RFCOMM ---
    if is_rfcomm_bound(RFCOMM_PORT):
        print(f"[INFO] {RFCOMM_DEV} already exists — releasing")
        rfcomm_release(RFCOMM_PORT)
        time.sleep(0.5)

    # --- Step 5: Bind RFCOMM ---
    if not rfcomm_bind(mac, RFCOMM_PORT):
        print("[ERROR] RFCOMM bind failed")
        sys.exit(1)

    # --- Step 6: Verify ---
    time.sleep(0.5)
    if not os.path.exists(RFCOMM_DEV):
        print(f"[ERROR] {RFCOMM_DEV} does not exist after bind")
        sys.exit(1)

    print(f"\n{'='*50}")
    print(f"[OK] ESP32-FTP ready on {RFCOMM_DEV}")
    print(f"     MAC: {mac}")
    print(f"{'='*50}\n")

    # --- Step 7: Launch client ---
    if args.client:
        client_path = os.path.join(os.path.dirname(__file__), "python_client", "bt_ftp_client.py")
        if not os.path.exists(client_path):
            # Fallback: look in same dir
            client_path = "bt_ftp_client.py"
        print(f"[INFO] Launching: python3 {client_path} {RFCOMM_DEV}")
        os.execvp("python3", ["python3", client_path, RFCOMM_DEV])
    else:
        print("Next step:")
        print(f"  python3 python_client/bt_ftp_client.py {RFCOMM_DEV}")


if __name__ == "__main__":
    main()
