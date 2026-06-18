#!/usr/bin/env python3
"""TUI / CLI front-end for ESP32-CAM serial file transfer."""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_esp32cam import ESP32CamSerial, ESP32CamError


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ESP32-CAM Serial File Transfer Client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s /dev/ttyACM0 repl
  %(prog)s -v /dev/ttyACM0 list /sdcard
  %(prog)s /dev/ttyACM0 upload photo.jpg /sdcard/picture99.jpg
  %(prog)s /dev/ttyACM0 download /sdcard/picture1.jpg ./saved.jpg
""",
    )
    p.add_argument("port", help="Serial port (e.g. /dev/ttyACM0, COM3)")
    p.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose debug output from the serial library",
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

    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    cam = ESP32CamSerial(args.port, verbose=args.verbose)

    print(f"[tui] Connecting to {args.port}...")
    if args.verbose:
        print("[tui] Verbose mode enabled")
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
