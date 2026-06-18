#!/usr/bin/env python3
"""CLI front-end for ESP32-CAM serial file transfer (XMODEM-CRC)."""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from xmodem_transfer import ESP32Serial, ESP32CamError


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ESP32-CAM Serial File Transfer Client (XMODEM-CRC)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s /dev/ttyUSB0 repl
  %(prog)s -v /dev/ttyUSB0 list /sdcard
  %(prog)s --baud 115200 /dev/ttyUSB0 list /sdcard
  %(prog)s /dev/ttyUSB0 upload photo.jpg /sdcard/picture99.jpg
  %(prog)s /dev/ttyUSB0 download /sdcard/picture1.jpg ./saved.jpg
  %(prog)s /dev/ttyUSB0 delete /sdcard/old.jpg
  %(prog)s /dev/ttyUSB0 mode photo
""",
    )
    p.add_argument("port", help="Serial port (e.g. /dev/ttyUSB0, COM3, /dev/cu.usbserial-*)")
    p.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose debug output",
    )
    p.add_argument(
        "--baud",
        type=int,
        default=921600,
        help="Transfer baud rate (default: 921600 for CH340; try 2000000 for FT232)",
    )
    sub = p.add_subparsers(dest="command", help="Command to run")

    sub.add_parser("repl", help="Interactive REPL")

    p_list = sub.add_parser("list", help="List files")
    p_list.add_argument("path", nargs="?", default="/sdcard", help="Directory path (default: /sdcard)")

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


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    cam = ESP32Serial(args.port, transfer_baud=args.baud, verbose=args.verbose)

    print(f"[client] Connecting to {args.port} at {args.baud} baud...")
    try:
        cam.connect()
        print(f"[client] Connected at {cam.baud} baud.")
    except ConnectionError as e:
        print(f"[client] Failed to connect: {e}")
        return 1

    try:
        if args.command == "repl":
            cam.repl()

        elif args.command == "list":
            print(f"[client] Listing '{args.path}'...")
            entries = cam.list_files(args.path)
            print(f"[client] {len(entries)} entries")
            for e in entries:
                if e["type"] == "dir":
                    print(f"  [DIR]  {e['name']}")
                else:
                    print(f"  [FILE] {e['size']:>8} bytes  {e['name']}")

        elif args.command == "delete":
            print(f"[client] Deleting '{args.path}'...")
            cam.delete_file(args.path)
            print(f"[client] Deleted.")

        elif args.command == "upload":
            print(f"[client] Reading '{args.src}'...")
            with open(args.src, "rb") as f:
                data = f.read()
            print(f"[client] Uploading {len(data)} bytes -> '{args.dst}'...")
            cam.store_file(args.dst, data)
            print(f"[client] Uploaded.")

        elif args.command == "download":
            print(f"[client] Downloading '{args.src}' -> '{args.dst}'...")
            data = cam.read_file(args.src)
            with open(args.dst, "wb") as f:
                f.write(data)
            print(f"[client] Downloaded {len(data)} bytes.")

        elif args.command == "mode":
            print(f"[client] Switching to {args.mode.upper()} mode...")
            cam.switch_mode(args.mode)
            print(f"[client] Mode switch acknowledged.")

    except ESP32CamError as e:
        print(f"[client] Device error: {e}")
        return 1
    except FileNotFoundError as e:
        print(f"[client] File not found: {e}")
        return 1
    except KeyboardInterrupt:
        print("\n[client] Interrupted.")
    finally:
        if cam.ser and cam.ser.is_open:
            print("[client] Disconnecting...")
            cam.disconnect()
        print("[client] Done.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
