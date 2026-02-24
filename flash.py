#!/usr/bin/env python3
"""
RNodeTHV4 Flash Utility

Flash the RNodeTHV4 boundary node firmware to a Heltec WiFi LoRa 32 V4.
No PlatformIO required — just Python 3 and a USB cable.

Usage:
    # Flash a pre-built merged binary (from GitHub Releases or local build)
    python flash.py

    # Flash a specific file
    python flash.py --file rnodethv4_firmware.bin

    # Download latest from GitHub and flash
    python flash.py --download

    # Specify serial port manually
    python flash.py --port /dev/ttyACM0

    # Just build the merged binary (requires PlatformIO build output)
    python flash.py --merge-only
"""

import argparse
import glob
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time

# ── Configuration ──────────────────────────────────────────────────────────────

CHIP            = "esp32s3"
FLASH_MODE      = "qio"
FLASH_FREQ      = "80m"
FLASH_SIZE      = "16MB"
BAUD_RATE       = "921600"
MERGED_FILENAME = "rnodethv4_firmware.bin"
GITHUB_REPO     = "jrl290/RNodeTHV4"

# Flash addresses for ESP32-S3 Arduino framework
BOOTLOADER_ADDR = 0x0000
PARTITIONS_ADDR = 0x8000
BOOT_APP0_ADDR  = 0xe000
APP_ADDR        = 0x10000

# PlatformIO build output paths (relative to project root)
BUILD_DIR       = ".pio/build/heltec_V4_boundary"
BOOTLOADER_BIN  = os.path.join(BUILD_DIR, "bootloader.bin")
PARTITIONS_BIN  = os.path.join(BUILD_DIR, "partitions.bin")
FIRMWARE_BIN    = os.path.join(BUILD_DIR, "rnode_firmware_heltec32v4_boundary.bin")

# ESP32 partition table magic bytes (first two bytes of a partition table entry)
PARTITION_TABLE_MAGIC = b'\xaa\x50'


def is_merged_binary(firmware_path):
    """Check whether a firmware file is a merged binary (contains bootloader +
    partition table) or an app-only binary.

    Returns True for merged, False for app-only.
    """
    try:
        size = os.path.getsize(firmware_path)
        if size > 0x8002:
            with open(firmware_path, "rb") as f:
                f.seek(0x8000)
                return f.read(2) == PARTITION_TABLE_MAGIC
    except Exception:
        pass
    return False


def _find_in_platformio_or_release(build_path, release_name):
    """Find a file in the PlatformIO build output or the bundled Release/ dir."""
    # 1. PlatformIO build output
    if os.path.isfile(build_path):
        return build_path

    # 2. Bundled in Release/
    bundled = os.path.join(os.path.dirname(__file__), "Release", release_name)
    if os.path.isfile(bundled):
        return bundled

    return None


def find_boot_app0():
    """Find boot_app0.bin from PlatformIO framework packages.

    Handles versioned package directories (e.g. framework-arduinoespressif32@3.20009.0).
    """
    pio_dir = os.path.expanduser("~/.platformio/packages")

    # Try exact name first
    exact = os.path.join(pio_dir, "framework-arduinoespressif32",
                         "tools", "partitions", "boot_app0.bin")
    if os.path.isfile(exact):
        return exact

    # Try versioned directories
    if os.path.isdir(pio_dir):
        for name in sorted(os.listdir(pio_dir), reverse=True):
            if name.startswith("framework-arduinoespressif32"):
                candidate = os.path.join(pio_dir, name, "tools", "partitions", "boot_app0.bin")
                if os.path.isfile(candidate):
                    return candidate

    # Bundled fallback
    bundled = os.path.join(os.path.dirname(__file__), "Release", "boot_app0.bin")
    if os.path.isfile(bundled):
        return bundled

    return None


def find_bootloader():
    """Find bootloader.bin from PlatformIO build output or Release/ bundle."""
    return _find_in_platformio_or_release(BOOTLOADER_BIN, "bootloader.bin")


def find_partitions():
    """Find partitions.bin from PlatformIO build output or Release/ bundle."""
    return _find_in_platformio_or_release(PARTITIONS_BIN, "partitions.bin")


BOOT_APP0_BIN = find_boot_app0()

# ── Helpers ────────────────────────────────────────────────────────────────────

def find_esptool():
    """Find esptool.py — pip-installed, bundled, or PlatformIO's copy.

    Prefer pip-installed esptool first (handles its own deps), then fall
    back to the bundled script — but only if pyserial is importable in
    the current Python interpreter.
    """
    # 1. pip-installed esptool (standalone executable, no dep issues)
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    if shutil.which("esptool"):
        return ["esptool"]

    # Check if pyserial is available before using script-based esptool
    try:
        import serial  # noqa: F401
        has_pyserial = True
    except ImportError:
        has_pyserial = False

    # 2. Bundled in Release/
    bundled = os.path.join(os.path.dirname(__file__), "Release", "esptool", "esptool.py")
    if os.path.isfile(bundled) and has_pyserial:
        return [sys.executable, bundled]

    # 3. PlatformIO's esptool
    pio_esptool = os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py"
    )
    if os.path.isfile(pio_esptool) and has_pyserial:
        return [sys.executable, pio_esptool]

    # 4. Bundled exists but pyserial is missing — tell the user
    if os.path.isfile(bundled) and not has_pyserial:
        print("Found bundled esptool but pyserial is not installed.")
        print("Install it with:  pip install pyserial")
        print("Or install the standalone esptool:  pip install esptool")
        sys.exit(1)

    return None


def find_serial_port():
    """List available serial ports and let the user choose."""
    system = platform.system()

    # Gather ports from glob patterns
    if system == "Darwin":
        patterns = ["/dev/cu.usbmodem*", "/dev/tty.usbmodem*",
                    "/dev/cu.usbserial*", "/dev/cu.SLAB*"]
    elif system == "Linux":
        patterns = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    else:
        patterns = []

    ports = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))

    # Also try pyserial's port enumeration (works on all platforms including Windows)
    try:
        import serial.tools.list_ports
        for port in serial.tools.list_ports.comports():
            if port.device not in ports:
                ports.append(port.device)
    except ImportError:
        pass

    # Sort for consistent ordering
    ports.sort()

    if not ports:
        return None

    print("\nAvailable serial ports:")
    for i, p in enumerate(ports):
        print(f"  [{i+1}] {p}")
    print()

    while True:
        try:
            choice = input(f"Select port [1-{len(ports)}]: ").strip()
            idx = int(choice) - 1
            if 0 <= idx < len(ports):
                return ports[idx]
        except (ValueError, EOFError):
            pass
        print("Invalid selection, try again.")


def sha256_file(path):
    """Compute SHA-256 hash of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def download_firmware(dest_path):
    """Download the latest merged firmware from GitHub Releases."""
    try:
        from urllib.request import urlretrieve, urlopen
        import json
    except ImportError:
        print("Error: Python urllib not available.")
        return False

    api_url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    print(f"Checking latest release from {GITHUB_REPO}...")

    try:
        with urlopen(api_url) as resp:
            release = json.loads(resp.read())
    except Exception as e:
        print(f"Error fetching release info: {e}")
        return False

    # Find the merged firmware asset
    asset_url = None
    for asset in release.get("assets", []):
        if asset["name"] == MERGED_FILENAME:
            asset_url = asset["browser_download_url"]
            break

    if not asset_url:
        print(f"Error: '{MERGED_FILENAME}' not found in latest release ({release.get('tag_name', '?')}).")
        print("Available assets:", [a["name"] for a in release.get("assets", [])])
        return False

    print(f"Downloading {release['tag_name']} / {MERGED_FILENAME}...")
    try:
        urlretrieve(asset_url, dest_path)
    except Exception as e:
        print(f"Download failed: {e}")
        return False

    size = os.path.getsize(dest_path)
    print(f"Downloaded {size:,} bytes  SHA-256: {sha256_file(dest_path)[:16]}...")
    return True


def _do_merge(output_path, esptool_cmd, bootloader, partitions, boot_app0, firmware):
    """Low-level merge: combine the four components into a single binary."""
    print("Merging firmware components...")
    print(f"  Bootloader: {bootloader}  @ 0x{BOOTLOADER_ADDR:04x}")
    print(f"  Partitions: {partitions}  @ 0x{PARTITIONS_ADDR:04x}")
    print(f"  boot_app0:  {boot_app0}   @ 0x{BOOT_APP0_ADDR:04x}")
    print(f"  Firmware:   {firmware}    @ 0x{APP_ADDR:05x}")

    cmd = esptool_cmd + [
        "--chip", CHIP,
        "merge_bin",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
        "-o", output_path,
        f"0x{BOOTLOADER_ADDR:x}", bootloader,
        f"0x{PARTITIONS_ADDR:x}", partitions,
        f"0x{BOOT_APP0_ADDR:x}",  boot_app0,
        f"0x{APP_ADDR:x}",        firmware,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error merging: {result.stderr}{result.stdout}")
        return False

    size = os.path.getsize(output_path)
    print(f"\nMerged binary: {output_path} ({size:,} bytes)")
    print(f"SHA-256: {sha256_file(output_path)[:16]}...")
    return True


def merge_firmware(output_path, esptool_cmd):
    """Merge bootloader + partitions + boot_app0 + app into a single binary.

    Uses PlatformIO build output, falling back to bundled Release/ copies
    for the boot components.
    """
    bootloader = find_bootloader()
    partitions = find_partitions()
    boot_app0  = BOOT_APP0_BIN
    firmware   = FIRMWARE_BIN

    missing = []
    if not bootloader:         missing.append(("bootloader", BOOTLOADER_BIN))
    if not partitions:         missing.append(("partitions", PARTITIONS_BIN))
    if not boot_app0:          missing.append(("boot_app0",  "(not found)"))
    if not os.path.isfile(firmware):
        missing.append(("firmware", firmware))

    if missing:
        for name, path in missing:
            print(f"Error: {name} not found: {path}")
        print("Run 'pio run -e heltec_V4_boundary' to build first.")
        return False

    return _do_merge(output_path, esptool_cmd, bootloader, partitions, boot_app0, firmware)


def auto_merge_app_binary(app_binary_path, esptool_cmd):
    """Auto-merge an app-only binary with boot components for a full flash.

    Finds bootloader, partitions, and boot_app0 from PlatformIO build output
    or the bundled Release/ directory, then merges them with the supplied
    app binary into a temporary merged file.

    Returns the path to the merged binary on success, or None on failure.
    """
    bootloader = find_bootloader()
    partitions = find_partitions()
    boot_app0  = BOOT_APP0_BIN

    missing = []
    if not bootloader: missing.append("bootloader.bin")
    if not partitions: missing.append("partitions.bin")
    if not boot_app0:  missing.append("boot_app0.bin")

    if missing:
        print(f"Cannot auto-merge: missing {', '.join(missing)}")
        print("Place them in the Release/ folder alongside flash.py, or")
        print("build with PlatformIO: pio run -e heltec_V4_boundary")
        return None

    # Create merged binary next to the app binary
    base, ext = os.path.splitext(app_binary_path)
    merged_path = f"{base}_merged{ext}"

    print("Auto-merging app-only binary with boot components...")
    if _do_merge(merged_path, esptool_cmd, bootloader, partitions, boot_app0, app_binary_path):
        return merged_path
    return None


def reset_to_bootloader(port):
    """Open serial port at 1200 baud to trigger ESP32-S3 USB bootloader reset.

    Many ESP32-S3 boards with native USB will enter download mode when
    the port is opened at 1200 baud with DTR toggled. This is useful
    when the device is stuck or unresponsive to normal esptool connection.
    """
    try:
        import serial
    except ImportError:
        print("Error: pyserial is required for 1200 baud reset.")
        print("Install it with:  pip install pyserial")
        return False

    print(f"Opening {port} at 1200 baud to trigger bootloader...")
    try:
        ser = serial.Serial(port, 1200)
        ser.dtr = False
        time.sleep(0.1)
        ser.dtr = True
        time.sleep(0.1)
        ser.dtr = False
        ser.close()
    except Exception as e:
        print(f"Error: {e}")
        return False

    print("Waiting for device to re-enumerate in download mode...")
    time.sleep(3)
    print("Done. The device should now be in download mode.")
    return True


def flash_firmware(firmware_path, port, esptool_cmd, baud=BAUD_RATE):
    """Flash firmware to the device."""
    print(f"\nFlashing {firmware_path} to {port}...")
    print(f"  Chip: {CHIP}  Baud: {baud}  Flash: {FLASH_SIZE}\n")

    # Determine if this is a merged binary (flash at 0x0) or app-only (flash at 0x10000)
    is_merged = is_merged_binary(firmware_path)

    if is_merged:
        flash_addr = f"0x{BOOTLOADER_ADDR:x}"
        print(f"  Detected: merged binary (partition table at 0x8000) -> flash at {flash_addr}")
    else:
        flash_addr = f"0x{APP_ADDR:x}"
        print(f"  Detected: app-only binary -> flash at {flash_addr}")

    cmd = esptool_cmd + [
        "--chip", CHIP,
        "--port", port,
        "--baud", baud,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "-z",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
        flash_addr, firmware_path,
    ]

    print("Running: " + " ".join(cmd[-8:]))
    result = subprocess.run(cmd)
    return result.returncode == 0


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="RNodeTHV4 Flash Utility — flash boundary node firmware to Heltec V4",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python flash.py                         # Flash local merged binary
  python flash.py --download              # Download latest release and flash
  python flash.py --file firmware.bin     # Flash a specific file
  python flash.py --merge-only            # Build merged binary from PlatformIO output
  python flash.py --port /dev/ttyACM0     # Specify serial port
  python flash.py --erase --download      # Erase flash, then download and flash
        """,
    )
    parser.add_argument("--file", "-f", help="Path to firmware binary to flash")
    parser.add_argument("--port", "-p", help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", "-b", default=BAUD_RATE, help=f"Baud rate (default: {BAUD_RATE})")
    parser.add_argument("--download", "-d", action="store_true",
                        help="Download latest firmware from GitHub Releases")
    parser.add_argument("--merge-only", action="store_true",
                        help="Merge PlatformIO build output into single binary, don't flash")
    parser.add_argument("--no-merge", action="store_true",
                        help="Skip merge step, use existing merged binary or --file")
    parser.add_argument("--erase", action="store_true",
                        help="Erase entire flash before writing (recommended for recovery)")

    args = parser.parse_args()
    baud = args.baud

    print("╔══════════════════════════════════════════╗")
    print("║       RNodeTHV4 Flash Utility            ║")
    print("║  Heltec WiFi LoRa 32 V4 Boundary Node   ║")
    print("╚══════════════════════════════════════════╝")
    print()

    # Find esptool
    esptool_cmd = find_esptool()
    if not esptool_cmd:
        print("Error: esptool not found!")
        print("Install it with:  pip install esptool")
        sys.exit(1)
    print(f"Using esptool: {' '.join(esptool_cmd)}")

    # Determine firmware file
    firmware_path = None

    if args.file:
        firmware_path = args.file
        if not os.path.isfile(firmware_path):
            print(f"Error: file not found: {firmware_path}")
            sys.exit(1)

    elif args.download:
        firmware_path = MERGED_FILENAME
        if not download_firmware(firmware_path):
            sys.exit(1)

    elif args.merge_only:
        if merge_firmware(MERGED_FILENAME, esptool_cmd):
            print(f"\nDone! Flash with:  python flash.py --file {MERGED_FILENAME}")
        else:
            sys.exit(1)
        return

    else:
        # Try to find or create a merged binary
        if os.path.isfile(MERGED_FILENAME) and not args.no_merge:
            # Check if PlatformIO build is newer
            if os.path.isfile(FIRMWARE_BIN):
                build_time = os.path.getmtime(FIRMWARE_BIN)
                merge_time = os.path.getmtime(MERGED_FILENAME)
                if build_time > merge_time:
                    print("Build output is newer than merged binary, re-merging...")
                    if not merge_firmware(MERGED_FILENAME, esptool_cmd):
                        sys.exit(1)
            firmware_path = MERGED_FILENAME
        elif os.path.isfile(FIRMWARE_BIN):
            # Build exists but no merged binary — create one
            print("Found PlatformIO build output, creating merged binary...")
            if not merge_firmware(MERGED_FILENAME, esptool_cmd):
                sys.exit(1)
            firmware_path = MERGED_FILENAME
        elif os.path.isfile(MERGED_FILENAME):
            firmware_path = MERGED_FILENAME
        else:
            print("No firmware found!")
            print()
            print("Options:")
            print("  1. Build with PlatformIO first:  pio run -e heltec_V4_boundary")
            print("  2. Download from GitHub:         python flash.py --download")
            print("  3. Specify a file:               python flash.py --file <path>")
            sys.exit(1)

    # Flash
    port = args.port or find_serial_port()
    if not port:
        print("\nError: No serial port detected!")
        print("Connect your Heltec V4 via USB and try again,")
        print("or specify manually: python flash.py --port /dev/ttyACM0")
        sys.exit(1)

    print(f"\nSerial port: {port}")
    print(f"Firmware:    {firmware_path} ({os.path.getsize(firmware_path):,} bytes)")
    print()

    # ── Interactive options ─────────────────────────────────────────────────

    # Offer 1200 baud reset if device might be stuck
    try:
        reset_choice = input("Reset device to download mode first? (try if device is stuck) [y/N] ").strip().lower()
    except EOFError:
        reset_choice = ""
    if reset_choice == "y":
        reset_to_bootloader(port)
        # Port may change after reset — re-scan
        print("Re-scanning serial ports (port may have changed)...")
        new_port = args.port or find_serial_port()
        if new_port:
            port = new_port
            print(f"Using port: {port}")
        else:
            print(f"Warning: No ports found after reset. Continuing with {port}")

    # Offer erase unless --erase was already passed
    if not args.erase:
        try:
            erase_choice = input("Erase flash before writing? (recommended for recovery) [y/N] ").strip().lower()
        except EOFError:
            erase_choice = ""
        if erase_choice == "y":
            args.erase = True

    # ── Safety check: erase + app-only → auto-merge ────────────────────────
    if args.erase and not is_merged_binary(firmware_path):
        print()
        print("╔══════════════════════════════════════════════════════════════╗")
        print("║  Erase selected with app-only binary — auto-merging boot   ║")
        print("║  components (bootloader + partition table + boot_app0) so   ║")
        print("║  the device remains bootable after erase.                  ║")
        print("╚══════════════════════════════════════════════════════════════╝")
        print()
        merged = auto_merge_app_binary(firmware_path, esptool_cmd)
        if merged:
            firmware_path = merged
            print(f"\nUsing auto-merged binary: {firmware_path}")
            print(f"  Size: {os.path.getsize(firmware_path):,} bytes")
            print()
        else:
            print()
            print("Auto-merge failed. Options:")
            print("  1) Skip erase and flash app-only (preserves existing NVS/bootloader)")
            print("  2) Abort")
            try:
                fallback = input("\nSkip erase and continue with app-only flash? [Y/n] ").strip().lower()
            except EOFError:
                fallback = ""
            if fallback == "n":
                print("Aborted.")
                sys.exit(1)
            args.erase = False
            print("Erase skipped. Continuing with app-only flash...\n")

    confirm = input("\nFlash firmware? [Y/n] ").strip().lower()
    if confirm and confirm != "y":
        print("Aborted.")
        sys.exit(0)

    if args.erase:
        print(f"Erasing flash on {port}...")
        erase_cmd = esptool_cmd + [
            "--chip", CHIP,
            "--port", port,
            "--baud", baud,
            "erase_flash",
        ]
        result = subprocess.run(erase_cmd)
        if result.returncode != 0:
            print("\nErase FAILED.")
            sys.exit(1)
        print("Flash erased. Waiting for device to re-enumerate...")
        time.sleep(3)

    if flash_firmware(firmware_path, port, esptool_cmd, baud):
        print()
        print("╔══════════════════════════════════════════╗")
        print("║          Flash complete!                 ║")
        print("║  Device will reboot automatically.       ║")
        print("║                                          ║")
        print("║  On first boot, hold PRG > 5s to enter   ║")
        print("║  the configuration portal.               ║")
        print("╚══════════════════════════════════════════╝")
    else:
        print("\nFlash FAILED. Check connection and try again.")
        print("You may need to hold BOOT while pressing RESET.")
        sys.exit(1)


if __name__ == "__main__":
    main()
