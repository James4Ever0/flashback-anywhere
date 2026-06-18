# Bluetooth Connection Debug Guide

## Problem: Pairing succeeds but connection drops immediately

This is the #1 issue with ESP32 Bluetooth SPP. The root cause is almost always **stale bonding keys**.

---

## Root Cause: Bonding Key Mismatch

When you pair a Bluetooth device, both sides store cryptographic bonding keys. If you **reflash the ESP32**, its internal bonding database is erased or reset. But your **PC still has the old keys**. The PC tries to reconnect using the old keys, authentication fails silently, and the connection drops immediately.

**Symptoms:**
- `onAuthComplete(true)` fires (pairing succeeds)
- `onConfirmRequest` does NOT fire (numeric comparison skipped — PC uses cached keys)
- `hasClient()` flips true then false within seconds
- Ubuntu shows "Connected" then immediately "Disconnected"

---

## Fix Steps (do ALL of these)

### 1. Delete the pairing on Ubuntu

```bash
# Find your ESP32 in the list
bluetoothctl devices

# Example output:
# Device AA:BB:CC:DD:EE:FF ESP32-FTP

# Remove it
bluetoothctl remove AA:BB:CC:DD:EE:FF

# Or remove ALL paired devices (nuclear option)
bluetoothctl paired-devices | while read line; do
    mac=$(echo $line | awk '{print $2}')
    [ -n "$mac" ] && bluetoothctl remove "$mac"
done
```

Also check the GUI: **Settings → Bluetooth → ESP32-FTP → Remove Device**

### 2. Flash the updated sketch

The updated code now does this on every boot:
```cpp
SerialBT.deleteAllBondedDevices();   // erase ESP32-side bonds
SerialBT.begin(BT_DEVICE_NAME, false, true);  // disableBLE=true
```

### 3. Pair fresh

1. Power cycle the ESP32 (unplug/replug USB for full reset)
2. In Ubuntu Bluetooth settings, find "ESP32-FTP"
3. Click **Pair** (not just Connect)
4. If a 6-digit code appears:
   - `BT_SSP_AUTO_CONFIRM=true`: ESP32 auto-accepts (check Serial Monitor)
   - `BT_SSP_AUTO_CONFIRM=false`: press BOOT button (GPIO0) on ESP32
5. After pairing succeeds, **DO NOT use Ubuntu Settings to "Connect"**

### 4. Use a serial terminal app — NOT Ubuntu Settings

**Ubuntu Bluetooth Settings is NOT a serial terminal.** It pairs the device but cannot open an RFCOMM serial session. The "Connect" button in Settings just probes the service and drops.

Use one of these:

**Option A: Serial Bluetooth Terminal (Android app)**
- Install from Play Store
- Pair with ESP32-FTP
- Connect, send `HELP`

**Option B: rfcomm + minicom (Linux)**
```bash
# Bind the ESP32 MAC to a virtual serial port
sudo rfcomm bind 0 AA:BB:CC:DD:EE:FF

# Check it exists
ls -la /dev/rfcomm0

# Open with minicom or screen
minicom -D /dev/rfcomm0 -b 115200
# or
screen /dev/rfcomm0 115200
```

**Option C: Python client**
```bash
cd bluetooth-ftp/python_client
python bt_ftp_client.py /dev/rfcomm0
```

**Option D: bluetoothctl connect**
```bash
bluetoothctl
[bluetooth]# pair AA:BB:CC:DD:EE:FF
[bluetooth]# trust AA:BB:CC:DD:EE:FF
[bluetooth]# connect AA:BB:CC:DD:EE:FF
```

---

## Expected Serial Monitor Output (Normal Flow)

```
========================================
[BOOT] Bluetooth FTP Server starting...
[INFO] SD mounted in 4-bit mode.
[INFO] No app-level PIN file found.
[INFO] Bluetooth 'ESP32-FTP' ready. MAC: AA:BB:CC:DD:EE:FF
[INFO] Deleting 1 old bonded device(s)...
[WARN] BT_SSP_AUTO_CONFIRM is true — any device can pair!
[INFO] FTP_AUTH_REQUIRED is false — open access after BT pairing.
========================================

[BT] Pairing SUCCESS (count=1)          ← only once for fresh pair
[BT] Client connected (hasClient=true)  ← serial app opens RFCOMM
[BT] Heartbeat: client still connected, available=0
```

---

## If It Still Drops

### Check 1: Is it a PC-side or ESP32-side drop?

Run this in a terminal while connecting:
```bash
# Monitor Bluetooth daemon logs
journalctl -u bluetooth -f
```

Look for:
- `Connection refused` → SPP service not registered properly
- `Authentication failed` → bond key mismatch (do the remove steps again)
- `Host is down` → ESP32 crashed or WiFi/BT conflict

### Check 2: Is the SPP service actually registered?

```bash
# After pairing, check if SPP (Serial Port) service is listed
sdptool browse AA:BB:CC:DD:EE:FF
```

You should see a `Serial Port` service with a channel number. If not, the ESP32 SPP init failed.

### Check 3: Try disabling WiFi

On ESP32-CAM, WiFi and Bluetooth share the radio. If WiFi is active, it can starve Bluetooth:
```cpp
// Add this in setup() before Bluetooth init:
WiFi.mode(WIFI_OFF);
```

### Check 4: Try a lower baud (doesn't affect BT but eliminates serial confusion)

Some terminal apps get confused. The BT virtual port ignores baud rate, but try:
```bash
python bt_ftp_client.py /dev/rfcomm0 9600
```

### Check 5: Use the PTY mock to verify the client works

If the mock server works but real hardware doesn't, the issue is Bluetooth-specific:
```bash
# Terminal A
python bluetooth-ftp/python_mock_bluetooth_server/mock_esp32_bt_ftp.py

# Terminal B (use the /dev/pts/N printed above)
python bluetooth-ftp/python_client/bt_ftp_client.py /dev/pts/7
```

---

## Quick Reference: ESP32-Side Debug Prints

| Message | Meaning |
|---------|---------|
| `[BT] Client connected (hasClient=true)` | RFCOMM serial session is open |
| `[BT] Client disconnected (hasClient=false)` | RFCOMM session closed |
| `[BT] Pairing SUCCESS (count=N)` | Bonding completed (N increments if called multiple times) |
| `[BT] Auto-confirming SSP code 123456` | Dev mode: accepted pairing without button |
| `[BT] Heartbeat: client still connected` | Connection alive, no data pending |
| `[BT] PAIRING REQUEST...` | Production mode: waiting for GPIO0 press |

---

## Code Changes Made (for reference)

1. `SerialBT.begin(BT_DEVICE_NAME, false, true)` — `disableBLE=true` prevents BLE from interfering with Classic BT
2. `SerialBT.deleteAllBondedDevices()` — clear stale keys on every boot
3. `SerialBT.getBtAddressString()` — print MAC for verification
4. Auth count in `onAuthComplete` — detect duplicate callbacks
5. Heartbeat debug prints — confirm connection stays alive
6. Initial `> ` prompt sent on connect — so client sees ready state immediately
