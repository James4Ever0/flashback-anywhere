#!/usr/bin/env python3
"""Terminal UI for mounting a USB drive and writing device info."""

import shutil
import sys
import tempfile

import lib


def select_device(devices: list[dict]) -> dict:
    print("Available USB devices:")
    for i, dev in enumerate(devices, 1):
        size_mb = lib.get_device_size(dev) // (1024 * 1024)
        print(f"  {i}. {dev['device_path']} {dev['id']} ({size_mb} MB)")
        # print(f"  {i}. {dev['device_path']} ({size_mb} MB)")
    while True:
        choice = input("Select device (number): ").strip()
        if choice.isdigit():
            idx = int(choice) - 1
            if 0 <= idx < len(devices):
                return devices[idx]
        print("Invalid selection, try again.")


def prompt_name() -> str:
    while True:
        name = input("Enter device name: ").strip()
        if name:
            return name
        print("Name cannot be empty.")


def main() -> int:
    keyword = input("Filter keyword (optional): ").strip() or None

    devices = lib.list_usb_devices(keyword)
    if not devices:
        print("No USB devices found.")
        return 1

    devices = lib.filter_devices(devices)
    if not devices:
        print("No USB devices >= 128 MB found.")
        return 1

    device = select_device(devices)
    device_path = device["device_path"]
    name = prompt_name()
    device_uuid = lib.generate_uuid()

    print(f"Unmounting {device_path} ...")
    lib.unmount_device(device_path)

    mount_point = tempfile.mkdtemp(prefix="usb_mount_")
    try:
        print(f"Mounting {device_path} to {mount_point} ...")
        lib.mount_device(device_path, mount_point)

        print("Writing device_info.cfg ...")
        lib.write_device_info(mount_point, name, device_uuid)

        print("Done.")
    finally:
        print(f"Unmounting {device_path} ...")
        lib.unmount_device(device_path)
        shutil.rmtree(mount_point)
    return 0


if __name__ == "__main__":
    sys.exit(main())
