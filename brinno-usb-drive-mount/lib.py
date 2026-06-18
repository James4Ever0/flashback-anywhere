import glob
import os
import subprocess
import uuid
from datetime import datetime

# check if user is root
if os.geteuid() != 0:
    exit("You need to have root privileges to run this script.\nPlease try again, this time using 'sudo'.")

def list_usb_devices(keyword: str | None = None) -> list[str]:
    """List USB block devices from /dev/disk/by-id/usb-*.

    Returns paths like /dev/sdX. If keyword is given, only entries
    whose filename contains the keyword (case-insensitive) are returned.
    """
    pattern = "/dev/disk/by-id/usb-*"
    paths = glob.glob(pattern)
    devices = []
    for p in paths:
        if keyword and keyword.lower() not in os.path.basename(p).lower():
            continue
        real = os.path.realpath(p)
        devices.append(dict(id=p, device_path=real))
    return sorted(devices, key=lambda x: x["device_path"])


def get_device_size(device: dict) -> int:
    """Return the size of a block device in bytes."""
    path = device["device_path"]
    with open("/sys/class/block/{}/size".format(os.path.basename(path))) as f:
        sectors = int(f.read().strip())
    return sectors * 512


def filter_devices(devices: list[dict]):
    devices = filter_devices_by_size(devices)
    devices = filter_devices_by_keyword(devices, "-part")
    return devices

def filter_devices_by_keyword(devices: list[dict], keyword: str = None) -> list[dict]:
    """Return devices whose filename contains keyword (case-insensitive)."""
    return [d for d in devices if keyword.lower() in os.path.basename(d["id"]).lower()]

def filter_devices_by_size(devices: list[dict], min_size_mb: int = 128) -> list[dict]:
    """Return devices whose size is >= min_size_mb."""
    min_bytes = min_size_mb * 1024 * 1024
    return [d for d in devices if get_device_size(d) >= min_bytes]


def unmount_device(device_path: str) -> None:
    """Unmount all mount points for the given device."""
    subprocess.run(["sudo", "umount", device_path], check=False, capture_output=True)


def mount_device(device_path: str, mount_point: str) -> None:
    """Mount device to mount_point, creating the directory if needed."""
    os.makedirs(mount_point, exist_ok=True)
    subprocess.run(["sudo", "mount", device_path, mount_point], check=True)


def generate_uuid() -> str:
    """Generate a random UUID string."""
    return str(uuid.uuid4())


def write_device_info(mount_point: str, name: str, device_uuid: str) -> None:
    """Write device_info.cfg into mount_point with name, uuid and last_sync_time."""
    now = datetime.now().isoformat()
    path = os.path.join(mount_point, "device_info.cfg")
    with open(path, "w") as f:
        f.write(f"name={name}\n")
        f.write(f"uuid={device_uuid}\n")
        f.write(f"last_sync_time={now}\n")
