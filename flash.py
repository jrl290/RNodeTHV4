#!/usr/bin/env python3
"""
RNodeTHV4 Flash Utility

Flash the RNodeTHV4 boundary node firmware to a Heltec WiFi LoRa 32 V3 or V4.
No PlatformIO required — just Python 3 and a USB cable.

By default, downloads the latest firmware from GitHub Releases (if newer than
the local cache) and flashes the app partition only, preserving bootloader,
partition table, NVS, and EEPROM settings.

Usage:
    # Update firmware — V4 (default)
    python flash.py

    # Update firmware — V3
    python flash.py --board v3

    # Flash a specific release version
    python flash.py --release v1.0.12

    # Full flash with merged binary (overwrites everything)
    python flash.py --full

    # Flash a specific file (auto-detects merged vs app-only)
    python flash.py --file firmware.bin

    # Specify serial port manually
    python flash.py --port /dev/ttyACM0

    # Skip online check — use cached/local firmware only
    python flash.py --offline

    # Just build the merged binary (for GitHub Releases)
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

VERSION         = "1.0.17"
CHIP            = "esp32s3"
FLASH_MODE      = "qio"
FLASH_FREQ      = "80m"
GITHUB_REPO     = "jrl290/RNodeTHV4"

# Flash addresses for ESP32-S3 Arduino framework
BOOTLOADER_ADDR = 0x0000
PARTITIONS_ADDR = 0x8000
BOOT_APP0_ADDR  = 0xe000
APP_ADDR        = 0x10000

# ── Board profiles ─────────────────────────────────────────────────────────────
# Each board defines its PIO env, flash size, baud rate, firmware binary name,
# and merged binary name.

BOARD_PROFILES = {
    "v4": {
        "name":            "Heltec WiFi LoRa 32 V4",
        "pio_env":         "heltec_V4_boundary",
        "build_dir":       ".pio/build/heltec_V4_boundary",
        "firmware_bin":    "rnode_firmware_heltec32v4_boundary.bin",
        "merged_filename": "rnodethv4_firmware.bin",
        "flash_size":      "16MB",
        "baud_rate":       "921600",
    },
    "v3": {
        "name":            "Heltec WiFi LoRa 32 V3",
        "pio_env":         "heltec_V3_boundary",
        "build_dir":       ".pio/build/heltec_V3_boundary",
        "firmware_bin":    "rnode_firmware_heltec32v3.bin",
        "merged_filename": "rnodethv3_firmware.bin",
        "flash_size":      "8MB",
        "baud_rate":       "460800",
    },
}
DEFAULT_BOARD = "v4"

# Active board profile (set in main() from --board arg)
_board = None

def board_profile():
    return BOARD_PROFILES[_board or DEFAULT_BOARD]

def BUILD_DIR():
    return board_profile()["build_dir"]

def BOOTLOADER_BIN():
    return os.path.join(BUILD_DIR(), "bootloader.bin")

def PARTITIONS_BIN():
    return os.path.join(BUILD_DIR(), "partitions.bin")

def FIRMWARE_BIN():
    return os.path.join(BUILD_DIR(), board_profile()["firmware_bin"])

def FLASH_SIZE():
    return board_profile()["flash_size"]

def BAUD_RATE():
    return board_profile()["baud_rate"]

def MERGED_FILENAME():
    return board_profile()["merged_filename"]

def PIO_ENV():
    return board_profile()["pio_env"]

# ESP32 partition table magic bytes (first two bytes of a partition table entry)
PARTITION_TABLE_MAGIC = b'\xaa\x50'
PARTITION_TABLE_SIZE  = 0xC00   # 3072 bytes


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


def extract_app_from_merged(merged_path):
    """Extract the app-only portion from a merged binary.

    A merged binary starts at 0x0000 and includes bootloader, partition table,
    boot_app0, and the app firmware.  The region between the partition table
    (0x8000-0x8BFF) and boot_app0 (0xE000) contains the NVS partition
    (0x9000-0xDFFF) which is filled with 0xFF padding by esptool merge_bin.
    Flashing a merged binary therefore wipes all saved settings.

    This function extracts bytes from APP_ADDR (0x10000) to the end of the
    file, producing an app-only binary that can be flashed at 0x10000 without
    touching NVS/EEPROM.

    Returns the path to the extracted app-only binary, or None on failure.
    """
    try:
        file_size = os.path.getsize(merged_path)
        if file_size <= APP_ADDR:
            print(f"  Warning: Merged binary too small ({file_size} bytes) to contain app data.")
            return None

        with open(merged_path, "rb") as f:
            f.seek(APP_ADDR)
            app_data = f.read()

        if not app_data:
            return None

        base, ext = os.path.splitext(merged_path)
        app_path = f"{base}_app{ext}"
        with open(app_path, "wb") as f:
            f.write(app_data)

        return app_path
    except Exception as e:
        print(f"  Warning: Could not extract app from merged binary: {e}")
        return None


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

# Forward-compatible aliases (these are now functions, not constants)
def _bootloader_bin():
    return BOOTLOADER_BIN()

def _partitions_bin():
    return PARTITIONS_BIN()

def _firmware_bin():
    return FIRMWARE_BIN()


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
    return _find_in_platformio_or_release(BOOTLOADER_BIN(), "bootloader.bin")


def find_partitions():
    """Find partitions.bin from PlatformIO build output or Release/ bundle."""
    return _find_in_platformio_or_release(PARTITIONS_BIN(), "partitions.bin")


BOOT_APP0_BIN = find_boot_app0()

# ── Board auto-detection ───────────────────────────────────────────────────────

# Map detected flash sizes to board keys
_FLASH_SIZE_TO_BOARD = {
    "16MB": "v4",
    "8MB":  "v3",
}

def detect_board(port, esptool_cmd):
    """Auto-detect which Heltec board is connected by querying flash size.

    Runs ``esptool.py flash_id`` and parses the output for:
      - Detected flash size (16MB → V4, 8MB → V3)
      - Chip type (ESP32-S3 expected)
      - Features (PSRAM size, WiFi, BLE)

    Returns a tuple (board_key, info_dict) on success, or (None, reason) on
    failure.  ``board_key`` is "v3" or "v4".
    """
    cmd = esptool_cmd + ["--port", port, "flash_id"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    except subprocess.TimeoutExpired:
        return None, "esptool timed out (device not responding?)"
    except Exception as e:
        return None, str(e)

    output = result.stdout + result.stderr
    if result.returncode != 0:
        return None, f"esptool flash_id failed:\n{output.strip()}"

    # Parse key fields
    info = {}
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("Chip is "):
            info["chip"] = line[len("Chip is "):]
        elif line.startswith("Features:"):
            info["features"] = line[len("Features:"):].strip()
        elif line.startswith("Detected flash size:"):
            info["flash_size"] = line.split(":")[-1].strip()
        elif line.startswith("MAC:"):
            info["mac"] = line.split(":")[-5:]  # last 5 colon-groups
            info["mac"] = line[len("MAC:"):].strip()
        elif line.startswith("Crystal is"):
            info["crystal"] = line[len("Crystal is"):].strip()

    flash_size = info.get("flash_size")
    if not flash_size:
        return None, f"Could not parse flash size from esptool output:\n{output.strip()}"

    board_key = _FLASH_SIZE_TO_BOARD.get(flash_size)
    if not board_key:
        return None, (
            f"Unknown flash size '{flash_size}' — expected 16MB (V4) or 8MB (V3).\n"
            f"Use --board v3 or --board v4 to specify manually."
        )

    return board_key, info


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


def _cache_dir():
    """Return the firmware cache directory (next to flash.py)."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), ".firmware_cache")


def _cache_meta_path(board_key):
    """Return path to the cache metadata JSON for a given board."""
    return os.path.join(_cache_dir(), board_key, "meta.json")


def _cached_firmware_path(board_key):
    """Return path to the cached firmware binary for a given board."""
    return os.path.join(_cache_dir(), board_key, BOARD_PROFILES[board_key]["merged_filename"])


def _read_cache_meta(board_key):
    """Read cache metadata, returning dict or None if not cached."""
    import json
    meta_path = _cache_meta_path(board_key)
    if os.path.isfile(meta_path):
        try:
            with open(meta_path) as f:
                return json.load(f)
        except Exception:
            pass
    return None


def _write_cache_meta(board_key, tag, sha256):
    """Write cache metadata after a successful download."""
    import json
    cache = os.path.join(_cache_dir(), board_key)
    os.makedirs(cache, exist_ok=True)
    meta = {"tag": tag, "sha256": sha256}
    with open(_cache_meta_path(board_key), "w") as f:
        json.dump(meta, f, indent=2)


def _parse_version_tag(tag):
    """Parse a version tag like 'v1.0.13' into a tuple (1, 0, 13) for comparison.
    Returns None if the tag doesn't match the expected format."""
    import re
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", tag)
    if m:
        return tuple(int(x) for x in m.groups())
    return None


def _fetch_release_info(tag=None):
    """Fetch release info from GitHub. If tag is None, fetches latest."""
    try:
        from urllib.request import urlopen, Request
        import json
    except ImportError:
        return None, "Python urllib not available"

    if tag:
        # Normalize: ensure tag starts with 'v'
        if not tag.startswith("v"):
            tag = f"v{tag}"
        api_url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/tags/{tag}"
    else:
        api_url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"

    try:
        req = Request(api_url, headers={"Accept": "application/vnd.github+json"})
        with urlopen(req, timeout=10) as resp:
            return json.loads(resp.read()), None
    except Exception as e:
        return None, str(e)


def fetch_firmware(board_key, release_tag=None):
    """Fetch firmware from GitHub, using cache when possible.

    Logic:
      1. Query GitHub for the target release (latest or specific tag).
      2. If the cached firmware matches that release tag, skip download.
      3. Otherwise download the merged firmware binary and update cache.

    Returns (firmware_path, release_tag) on success, (None, reason) on failure.
    """
    from urllib.request import urlretrieve

    merged_name = BOARD_PROFILES[board_key]["merged_filename"]
    cache_path = _cached_firmware_path(board_key)
    cache_meta = _read_cache_meta(board_key)

    # 1. Fetch release info
    label = f"release {release_tag}" if release_tag else "latest release"
    print(f"Checking {label} from {GITHUB_REPO}...")
    release, err = _fetch_release_info(release_tag)
    if not release:
        print(f"  Could not reach GitHub: {err}")
        # Fall back to cache if available
        if cache_meta and os.path.isfile(cache_path):
            print(f"  Using cached firmware: {cache_meta['tag']}")
            return cache_path, cache_meta["tag"]
        return None, f"No cached firmware and GitHub unreachable: {err}"

    remote_tag = release.get("tag_name", "unknown")

    # 2. Check cache
    if cache_meta and os.path.isfile(cache_path):
        cached_tag = cache_meta.get("tag")
        if cached_tag == remote_tag:
            # Verify file integrity
            actual_sha = sha256_file(cache_path)
            if actual_sha == cache_meta.get("sha256"):
                print(f"  Cached firmware is up-to-date: {remote_tag}")
                return cache_path, remote_tag
            else:
                print(f"  Cache integrity mismatch — re-downloading")
        else:
            cached_ver = _parse_version_tag(cached_tag) if cached_tag else None
            remote_ver = _parse_version_tag(remote_tag)
            if cached_ver and remote_ver and remote_ver > cached_ver:
                print(f"  Newer version available: {cached_tag} → {remote_tag}")
            elif cached_ver and remote_ver and remote_ver < cached_ver:
                print(f"  Requested version {remote_tag} is older than cached {cached_tag}")
            else:
                print(f"  Version changed: {cached_tag} → {remote_tag}")

    # 3. Find the asset URL
    asset_url = None
    for asset in release.get("assets", []):
        if asset["name"] == merged_name:
            asset_url = asset["browser_download_url"]
            break

    if not asset_url:
        available = [a["name"] for a in release.get("assets", [])]
        return None, (
            f"'{merged_name}' not found in release {remote_tag}.\n"
            f"  Available assets: {available}"
        )

    # 4. Download
    os.makedirs(os.path.join(_cache_dir(), board_key), exist_ok=True)
    print(f"  Downloading {remote_tag} / {merged_name}...")
    try:
        urlretrieve(asset_url, cache_path)
    except Exception as e:
        return None, f"Download failed: {e}"

    file_sha = sha256_file(cache_path)
    file_size = os.path.getsize(cache_path)
    _write_cache_meta(board_key, remote_tag, file_sha)
    print(f"  Downloaded {file_size:,} bytes  SHA-256: {file_sha[:16]}...")
    return cache_path, remote_tag


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
        "--flash_size", FLASH_SIZE(),
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
    firmware   = FIRMWARE_BIN()

    missing = []
    if not bootloader:         missing.append(("bootloader", BOOTLOADER_BIN()))
    if not partitions:         missing.append(("partitions", PARTITIONS_BIN()))
    if not boot_app0:          missing.append(("boot_app0",  "(not found)"))
    if not os.path.isfile(firmware):
        missing.append(("firmware", firmware))

    if missing:
        for name, path in missing:
            print(f"Error: {name} not found: {path}")
        print(f"Run 'pio run -e {PIO_ENV()}' to build first.")
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
        print(f"build with PlatformIO: pio run -e {PIO_ENV()}")
        return None

    # Create merged binary next to the app binary
    base, ext = os.path.splitext(app_binary_path)
    merged_path = f"{base}_merged{ext}"

    print("Auto-merging app-only binary with boot components...")
    if _do_merge(merged_path, esptool_cmd, bootloader, partitions, boot_app0, app_binary_path):
        return merged_path
    return None


def read_device_partitions(port, esptool_cmd, baud=None):
    """Read the partition table from the connected device.

    Uses esptool read_flash to read PARTITION_TABLE_SIZE bytes from
    PARTITIONS_ADDR (0x8000).

    Returns the raw bytes on success, or None on failure.
    """
    import tempfile
    if baud is None:
        baud = BAUD_RATE()

    tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    tmp.close()
    try:
        cmd = esptool_cmd + [
            "--chip", CHIP,
            "--port", port,
            "--baud", baud,
            "read_flash",
            f"0x{PARTITIONS_ADDR:x}",
            str(PARTITION_TABLE_SIZE),
            tmp.name,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            return None
        with open(tmp.name, "rb") as f:
            return f.read()
    except Exception:
        return None
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass


def check_partition_table(port, esptool_cmd, baud=None):
    """Compare the device's partition table against the expected one.

    Returns:
      True  — partition table matches (or no expected table to compare against)
      False — partition table mismatch (device needs full flash)
    """
    expected_path = find_partitions()
    if not expected_path:
        # Can't check — no reference partition table available
        return True

    with open(expected_path, "rb") as f:
        expected = f.read()

    print("Checking device partition table...")
    device_data = read_device_partitions(port, esptool_cmd, baud)
    if device_data is None:
        print("  Could not read partition table from device")
        # Can't verify — assume OK (user can always use --full)
        return True

    # Compare only the meaningful portion (both should be PARTITION_TABLE_SIZE)
    if device_data[:len(expected)] == expected:
        print("  Partition table OK ✓")
        return True

    # Check if device has any valid partition table at all
    if device_data[:2] != PARTITION_TABLE_MAGIC:
        print("  No valid partition table found on device (blank or corrupted)")
    else:
        print("  Partition table MISMATCH — device has a different layout")

    return False


def check_app_on_device(port, esptool_cmd, baud=None):
    """Check whether app firmware is present on the device.

    Reads a small chunk from APP_ADDR (0x10000).  If the region is all 0xFF
    (erased flash), no app is present and the device needs a full flash.

    Returns True if app firmware is detected, False if blank/absent.
    """
    import tempfile
    if baud is None:
        baud = BAUD_RATE()

    read_size = 256  # enough to distinguish blank from real firmware
    tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    tmp.close()
    try:
        cmd = esptool_cmd + [
            "--chip", CHIP,
            "--port", port,
            "--baud", baud,
            "read_flash",
            f"0x{APP_ADDR:x}",
            str(read_size),
            tmp.name,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print("  Warning: Could not read app region from device")
            return True  # assume app exists if we can't check
        with open(tmp.name, "rb") as f:
            data = f.read()
        # All 0xFF means flash is blank — no app present
        if data == b'\xff' * len(data):
            return False
        return True
    except Exception as e:
        print(f"  Warning: App check failed: {e}")
        return True  # assume app exists if we can't check
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass


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


def flash_firmware(firmware_path, port, esptool_cmd, baud=None):
    """Flash firmware to the device."""
    if baud is None:
        baud = BAUD_RATE()
    flash_size = FLASH_SIZE()
    print(f"\nFlashing {firmware_path} to {port}...")
    print(f"  Chip: {CHIP}  Baud: {baud}  Flash: {flash_size}\n")

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
        "--flash_size", flash_size,
        flash_addr, firmware_path,
    ]

    print("Running: " + " ".join(cmd[-8:]))
    result = subprocess.run(cmd)
    return result.returncode == 0


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    global _board
    parser = argparse.ArgumentParser(
        description="RNodeTHV4 Flash Utility — flash boundary node firmware to Heltec V3/V4",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python flash.py                         # Download latest & app-only update (V4)
  python flash.py --board v3              # Download latest & app-only update (V3)
  python flash.py --release v1.0.12       # Flash a specific release version
  python flash.py --full                  # Full flash with merged binary
  python flash.py --offline               # Use cached/local firmware only
  python flash.py --file firmware.bin     # Flash a specific file
  python flash.py --merge-only            # Build merged binary for release
  python flash.py --port /dev/ttyACM0     # Specify serial port
  python flash.py --erase --full          # Erase flash, then full flash
        """,
    )
    parser.add_argument("--board", choices=["v3", "v4"], default=None,
                        help="Target board: v3 (Heltec V3) or v4 (Heltec V4). "
                             "Auto-detected from connected device if omitted.")
    parser.add_argument("--file", "-f", help="Path to firmware binary to flash")
    parser.add_argument("--port", "-p", help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", "-b", default=None, help="Baud rate (board-specific default)")
    parser.add_argument("--release", "-r", default=None, metavar="TAG",
                        help="Flash a specific release version (e.g. v1.0.12)")
    parser.add_argument("--offline", action="store_true",
                        help="Skip online check — use cached or local firmware only")
    parser.add_argument("--merge-only", action="store_true",
                        help="Merge PlatformIO build output into single binary, don't flash")
    parser.add_argument("--full", action="store_true",
                        help="Flash merged binary (bootloader + partitions + app) — overwrites everything")
    parser.add_argument("--erase", action="store_true",
                        help="Erase entire flash before writing (implies --full)")

    args = parser.parse_args()

    # Find esptool early — needed for both auto-detect and flashing
    esptool_cmd = find_esptool()
    if not esptool_cmd:
        print("Error: esptool not found!")
        print("Install it with:  pip install esptool")
        sys.exit(1)

    # ── Board detection ─────────────────────────────────────────────────
    detected_info = None
    _early_port = None

    if args.board:
        # Explicit board — no detection needed
        _board = args.board
    elif args.merge_only:
        # No device needed for merge — fall back to default
        _board = DEFAULT_BOARD
        print(f"(No --board specified; defaulting to {DEFAULT_BOARD} for merge)")
    else:
        # Auto-detect from connected device
        _early_port = args.port or find_serial_port()
        if not _early_port:
            print("No serial port detected and no --board specified.")
            print(f"Defaulting to {DEFAULT_BOARD}. Specify with --board v3 or --board v4.")
            _board = DEFAULT_BOARD
        else:
            print(f"Detecting board on {_early_port}...")
            board_key, info = detect_board(_early_port, esptool_cmd)
            if board_key:
                _board = board_key
                detected_info = info
                print(f"  Chip:       {info.get('chip', '?')}")
                print(f"  Flash:      {info.get('flash_size', '?')}")
                print(f"  Features:   {info.get('features', '?')}")
                print(f"  MAC:        {info.get('mac', '?')}")
                print(f"  → Detected: {BOARD_PROFILES[board_key]['name']}")
            else:
                reason = info  # info is the error reason when board_key is None
                print(f"  Auto-detect failed: {reason}")
                print(f"  Defaulting to {DEFAULT_BOARD}. Specify with --board v3 or --board v4.")
                _board = DEFAULT_BOARD

    baud = args.baud or BAUD_RATE()
    bp = board_profile()

    print()
    print("╔══════════════════════════════════════════╗")
    print("║       RNodeTHV4 Flash Utility            ║")
    print(f"║  {bp['name']:^40s}  ║")
    print("╚══════════════════════════════════════════╝")
    print()
    print(f"Using esptool: {' '.join(esptool_cmd)}")

    # --erase implies --full (after erase, device needs bootloader + partitions)
    if args.erase:
        args.full = True

    # Determine firmware file
    firmware_path = None
    merged_fn = MERGED_FILENAME()
    firmware_bin = FIRMWARE_BIN()
    pio_env = PIO_ENV()

    if args.file:
        firmware_path = args.file
        if not os.path.isfile(firmware_path):
            print(f"Error: file not found: {firmware_path}")
            sys.exit(1)

    elif args.merge_only:
        if merge_firmware(merged_fn, esptool_cmd):
            print(f"\nDone! Flash with:  python flash.py --board {_board} --file {merged_fn}")
        else:
            sys.exit(1)
        return

    elif args.full and not args.release and args.offline:
        # Full flash, offline: use local PIO build or existing merged binary
        if os.path.isfile(firmware_bin):
            if os.path.isfile(merged_fn):
                build_time = os.path.getmtime(firmware_bin)
                merge_time = os.path.getmtime(merged_fn)
                if build_time > merge_time:
                    print("Build output is newer than merged binary, re-merging...")
                    if not merge_firmware(merged_fn, esptool_cmd):
                        sys.exit(1)
            else:
                print("Creating merged binary from PlatformIO build output...")
                if not merge_firmware(merged_fn, esptool_cmd):
                    sys.exit(1)
            firmware_path = merged_fn
        elif os.path.isfile(merged_fn):
            firmware_path = merged_fn
        else:
            # Try cache
            cached = _cached_firmware_path(_board)
            if os.path.isfile(cached):
                firmware_path = cached
                meta = _read_cache_meta(_board)
                print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
            else:
                print("No firmware found for full flash!")
                print()
                print("Options:")
                print(f"  1. Build with PlatformIO first:  pio run -e {pio_env}")
                print(f"  2. Run without --offline to download from GitHub")
                print(f"  3. Specify a file:               python flash.py --board {_board} --file <path>")
                sys.exit(1)

    else:
        # Default path: fetch from GitHub (unless --offline)
        if not args.offline:
            fw_path, tag_or_err = fetch_firmware(_board, release_tag=args.release)
            if fw_path:
                firmware_path = fw_path
                print(f"\n  Release: {tag_or_err}")
            else:
                print(f"\n  GitHub: {tag_or_err}")
                print("  Falling back to local firmware...")

        # Fall back to local PIO build output or cache
        if not firmware_path:
            if os.path.isfile(firmware_bin):
                firmware_path = firmware_bin
                print(f"Using local PlatformIO build: {firmware_bin}")
            else:
                cached = _cached_firmware_path(_board)
                if os.path.isfile(cached):
                    firmware_path = cached
                    meta = _read_cache_meta(_board)
                    print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
                elif os.path.isfile(merged_fn):
                    firmware_path = merged_fn
                    print(f"Using local merged binary: {merged_fn}")
                else:
                    print("No firmware found!")
                    print()
                    print("Options:")
                    print(f"  1. Build with PlatformIO first:  pio run -e {pio_env}")
                    print(f"  2. Specify a file:               python flash.py --board {_board} --file <path>")
                    sys.exit(1)

    # ── Device checks & flash decision ──────────────────────────────────────
    #
    # Flow:
    #   1. --full or --erase on CLI → full_flash = True
    #   2. Check if app firmware exists on device → if not → full_flash = True
    #   3. Check partition table matches expected → if not → full_flash = True
    #   4. Ask user "Erase flash before writing?" → Y → full_flash = True
    #   5. full_flash → flash merged binary at 0x0000
    #   6. Otherwise → extract app from merged, flash at 0x10000

    # Reuse early-detected port, or find one now
    port = args.port or _early_port or find_serial_port()
    if not port:
        print("\nError: No serial port detected!")
        print(f"Connect your {bp['name']} via USB and try again,")
        print(f"or specify manually: python flash.py --board {_board} --port /dev/ttyACM0")
        sys.exit(1)

    print(f"\nSerial port: {port}")
    print(f"Firmware:    {firmware_path} ({os.path.getsize(firmware_path):,} bytes)")

    full_flash = args.full or args.erase

    if not full_flash:
        print("\nChecking device state...")
        has_app = check_app_on_device(port, esptool_cmd, baud)
        if not has_app:
            print("  No app firmware on device — full flash required")
            full_flash = True

    if not full_flash:
        pt_ok = check_partition_table(port, esptool_cmd, baud)
        if not pt_ok:
            print("  Partition table mismatch — full flash required")
            full_flash = True

    if not full_flash:
        try:
            erase_choice = input("\nErase flash before writing? (wipes all settings) [y/N] ").strip().lower()
        except EOFError:
            erase_choice = ""
        if erase_choice == "y":
            full_flash = True

    # ── Prepare firmware based on flash decision ────────────────────────────
    if full_flash:
        # Need the merged binary — ensure we have one
        if not is_merged_binary(firmware_path):
            print("\nCreating merged binary for full flash...")
            merged = auto_merge_app_binary(firmware_path, esptool_cmd)
            if merged:
                firmware_path = merged
            else:
                print("  ERROR: Cannot create merged binary — missing boot components.")
                print(f"  Build with PlatformIO first:  pio run -e {PIO_ENV()}")
                sys.exit(1)

        print(f"\n  Full flash: {os.path.basename(firmware_path)} → 0x{BOOTLOADER_ADDR:04x}")
        print(f"  Size: {os.path.getsize(firmware_path):,} bytes")
        print(f"  ⚠  This will overwrite all settings (NVS/EEPROM)")
    else:
        # Extract app-only from merged binary to preserve settings
        if is_merged_binary(firmware_path):
            app_path = extract_app_from_merged(firmware_path)
            if app_path:
                firmware_path = app_path
            else:
                print("\n  ERROR: Could not extract app from merged binary.")
                sys.exit(1)

        print(f"\n  App-only update: {os.path.basename(firmware_path)} → 0x{APP_ADDR:05x}")
        print(f"  Size: {os.path.getsize(firmware_path):,} bytes")
        print(f"  WiFi/boundary settings will be preserved")

    # ── Interactive options ─────────────────────────────────────────────────

    # Offer 1200 baud reset if device might be stuck
    try:
        reset_choice = input("\nReset device to download mode first? (try if device is stuck) [y/N] ").strip().lower()
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

    confirm = input("\nFlash firmware? [Y/n] ").strip().lower()
    if confirm and confirm != "y":
        print("Aborted.")
        sys.exit(0)

    # ── Erase flash (only when --erase was explicitly passed) ───────────────
    if args.erase:
        print(f"\nErasing flash on {port}...")
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
