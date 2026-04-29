#!/usr/bin/env python3
"""
RTNode-HeltecV4 Flash Utility

Flash the RTNode-HeltecV4 transport node firmware to a Heltec WiFi LoRa 32 V3 or V4.
No PlatformIO required — just Python 3 and a USB cable.

By default, downloads the latest firmware from GitHub Releases (if newer than
the local cache) and flashes the app partition only, preserving bootloader,
partition table, NVS, and EEPROM settings. For reproducible flashing, the
script prefers the bundled esptool in Release/ over any host-installed copy.

Usage:
    # Update firmware — V4 (default)
    python flash.py

    # Legacy alias for an app-only update flow
    python flash.py --update

    # Use a host-installed esptool instead of the bundled copy
    python flash.py --use-system-esptool

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

VERSION         = "1.0.18"
CHIP            = "esp32s3"
FLASH_MODE      = "qio"    # Global default; overridden by board profile
FLASH_FREQ      = "80m"
GITHUB_REPO     = "jrl290/RTNode-HeltecV4"

# Runtime state (set automatically during main())
_flash_mode_override = None    # CLI --flash-mode sets this; otherwise board profile wins
_detected_flash_size = None    # Actual flash size read from device; overrides board profile
_esptool_write_verify_support = {}

# Flash addresses for ESP32-S3 Arduino framework
BOOTLOADER_ADDR = 0x0000
PARTITIONS_ADDR = 0x8000
BOOT_APP0_ADDR  = 0xe000
APP_ADDR        = 0x10000

# ── Board profiles ─────────────────────────────────────────────────────────────
# Each board defines its PIO env, flash size, baud rate, firmware binary name,
# and merged binary name.

# Single archive name released on GitHub — contains every board/flash-size variant.
FIRMWARE_ARCHIVE   = "rtnode_firmware.zip"
# Conservative default when flash size can't be detected: 8MB firmware runs on
# any device (8MB or larger); a 16MB image on an 8MB device crashes at boot.
DEFAULT_FLASH_SIZE = "8MB"

BOARD_PROFILES = {
    "v4": {
        "name":      "Heltec WiFi LoRa 32 V4",
        "chip":      "ESP32-S3",   # matches esptool "Chip is ESP32-S3 ..."
        "baud_rate": "921600",
        "flash_mode": "qio",  # ESP32-S3 PSRAM requires QIO — DIO disables PSRAM
        "flash_variants": {
            "16MB": {
                "pio_env":      "rtnode_heltec_v4",
                "build_dir":    ".pio/build/rtnode_heltec_v4",
                "firmware_bin": "rtnode_heltec_v4.bin",
                "merged_bin":   "rtnode_heltec_v4_merged.bin",
            },
        },
    },
    "v3": {
        "name":      "Heltec WiFi LoRa 32 V3",
        "chip":      "ESP32",      # matches esptool "Chip is ESP32 ..."
        "baud_rate": "460800",
        "flash_mode": "dio",
        "flash_variants": {
            "8MB": {
                "pio_env":      "rtnode_heltec_v3",
                "build_dir":    ".pio/build/rtnode_heltec_v3",
                "firmware_bin": "rtnode_heltec_v3.bin",
                "merged_bin":   "rtnode_heltec_v3_merged.bin",
            },
        },
    },
    # Tracker is ESP32-S3FN8 (embedded flash, NO PSRAM) — must use DIO flash mode.
    # Always select explicitly: python flash.py --board tracker
    "tracker": {
        "name":      "Heltec Wireless Tracker v1.1",
        "chip":      "ESP32-S3",
        "baud_rate": "921600",
        "flash_mode": "dio",
        "flash_variants": {
            "8MB": {
                "pio_env":      "rtnode_heltec_tracker",
                "build_dir":    ".pio/build/rtnode_heltec_tracker",
                "firmware_bin": "rtnode_heltec_tracker.bin",
                "merged_bin":   "rtnode_heltec_tracker_merged.bin",
            },
        },
    },
}
DEFAULT_BOARD = "v4"

# Active board profile (set in main() from --board arg)
_board = None

def board_profile():
    return BOARD_PROFILES[_board or DEFAULT_BOARD]

def flash_variant():
    """Return the flash variant dict for the active board and detected flash size.

    Falls back to the smallest (safest) available variant when the exact size is
    unknown — a smaller-flash firmware runs on any larger device, but not vice versa.
    """
    variants = board_profile()["flash_variants"]
    size = _detected_flash_size or DEFAULT_FLASH_SIZE
    if size in variants:
        return variants[size]
    # Fallback: smallest available variant
    available = sorted(variants.keys(), key=lambda s: int(s.replace("MB", "")))
    return variants[available[0]]

def BUILD_DIR():
    return flash_variant()["build_dir"]

def MERGED_BIN():
    """Return the path to the pre-merged binary in the PlatformIO build dir."""
    merged = flash_variant().get("merged_bin")
    if not merged:
        return None
    return os.path.join(BUILD_DIR(), merged)

def find_local_firmware():
    """Look for pre-built firmware binaries adjacent to flash.py.

    This is the primary path when the user has extracted the release ZIP.
    Prefers the pre-merged binary (full-flash-ready, no merge step needed)
    over the app-only binary.

    Returns the path if found, or None.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    fv = flash_variant()
    # Prefer merged binary — works for both full flash and app-only update
    merged = fv.get("merged_bin")
    if merged:
        path = os.path.join(script_dir, merged)
        if os.path.isfile(path):
            return path
    # Fall back to app-only binary
    path = os.path.join(script_dir, fv["firmware_bin"])
    if os.path.isfile(path):
        return path
    return None

def BOOTLOADER_BIN():
    return os.path.join(BUILD_DIR(), "bootloader.bin")

def PARTITIONS_BIN():
    return os.path.join(BUILD_DIR(), "partitions.bin")

def FIRMWARE_BIN():
    return os.path.join(BUILD_DIR(), flash_variant()["firmware_bin"])

def FLASH_SIZE():
    """Return the effective flash size: detected from device, or conservative default."""
    return _detected_flash_size or DEFAULT_FLASH_SIZE

def BAUD_RATE():
    return board_profile()["baud_rate"]

def BOARD_FLASH_MODE():
    """Return the effective flash mode for the current board.

    Priority: CLI override > board profile > global default.
    """
    return _flash_mode_override or board_profile().get("flash_mode", FLASH_MODE)

def PIO_ENV():
    return flash_variant()["pio_env"]

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
    """Find a file in the PlatformIO build output.

    The Release/ directory no longer ships pre-built boot component binaries.
    All boot components come from PlatformIO build output.  Call
    ensure_firmware_built() first to trigger a build when needed.
    """
    if os.path.isfile(build_path):
        return build_path
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

    return None


def find_bootloader():
    """Find bootloader.bin from PlatformIO build output or Release/ bundle."""
    return _find_in_platformio_or_release(BOOTLOADER_BIN(), "bootloader.bin")


def find_partitions():
    """Find partitions.bin from PlatformIO build output or Release/ bundle."""
    return _find_in_platformio_or_release(PARTITIONS_BIN(), "partitions.bin")


BOOT_APP0_BIN = find_boot_app0()

# ── Board auto-detection ───────────────────────────────────────────────────────

# Map chip type to board keys. Chip string comes from esptool "Chip is <type>".
# Sorted longest-first in detect_board so "ESP32-S3" wins over "ESP32".
_CHIP_TO_BOARD = {
    "ESP32-S3": "v4",
    "ESP32":    "v3",
}

def read_flash_info(port, esptool_cmd):
    """Read device flash metadata using ``esptool.py flash_id``.

    Returns ``(info_dict, None)`` on success, or ``(None, reason)`` on failure.
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

    return info, None


def detect_board(port, esptool_cmd):
    """Auto-detect which Heltec board is connected.

    Uses chip type as the primary discriminator (ESP32-S3 → V4, ESP32 → V3),
    so a V4 device with 8MB flash is correctly identified as V4, not V3.
    Flash size is stored in the returned info dict and used to select the
    correct firmware variant.

    Returns a tuple (board_key, info_dict) on success, or (None, reason) on
    failure.  ``board_key`` is "v3" or "v4".
    """
    info, err = read_flash_info(port, esptool_cmd)
    if not info:
        return None, err

    chip_str = info.get("chip", "")
    features = info.get("features", "")

    if "ESP32-S3" in chip_str:
        # Both V3 and V4 are ESP32-S3. Distinguish by PSRAM presence.
        # V4 (ESP32-S3FH4R2): features includes "Embedded PSRAM"
        # V3 (ESP32-S3FN8):   features has no PSRAM entry
        if "PSRAM" in features.upper():
            board_key = "v4"
        else:
            board_key = "v3"
    elif "ESP32" in chip_str:
        board_key = "v3"
    else:
        return None, (
            f"Unknown chip '{chip_str}' — expected ESP32-S3 (V3/V4) or ESP32.\n"
            f"Use --board v3 or --board v4 to specify manually."
        )

    return board_key, info


# ── Helpers ────────────────────────────────────────────────────────────────────

def find_esptool(prefer_system=False):
    """Find esptool, preferring the bundled standalone binary for reproducibility.

    Default search order:
      1. Release/esptool/esptool  (standalone binary — no Python/pyserial needed)
      2. PlatformIO packaged esptool.py  (dev environments)
      3. Host-installed esptool (PATH / ~/.local/bin)

    Pass ``prefer_system=True`` (--use-system-esptool) to move the host
    installation to the front, useful when you want to use a newer system
    esptool for debugging.

    The standalone binary MUST remain first in repo_candidates.
    """
    # Check if pyserial is available — needed for script-based (*.py) fallbacks
    try:
        import serial  # noqa: F401
        has_pyserial = True
    except ImportError:
        has_pyserial = False

    # Platform-aware standalone binary name
    exe_name   = "esptool.exe" if platform.system() == "Windows" else "esptool"
    script_dir = os.path.dirname(os.path.abspath(__file__))
    # Two possible locations for the standalone binary:
    #   1. <script_dir>/esptool/esptool   — flat ZIP extraction (user-facing release)
    #   2. <script_dir>/Release/esptool/esptool — git repo / nested ZIP
    bundled_bin = (
        os.path.join(script_dir, "esptool", exe_name)
        if os.path.isfile(os.path.join(script_dir, "esptool", exe_name))
        else os.path.join(script_dir, "Release", "esptool", exe_name)
    )
    pio_esptool = os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py"
    )

    repo_candidates = []
    # !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    # DO NOT CHANGE THIS ORDER.
    # The bundled Release/esptool binary is INTENTIONALLY first.
    # It is a platform-native standalone executable — no Python or pyserial
    # required.  Users flashing from the release archive get a consistent,
    # pinned esptool regardless of what is installed on their machine.
    # PlatformIO esptool.py is a fallback for dev environments only.
    # NEVER reorder these lines. NEVER "prefer" PlatformIO over bundled.
    # !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    if os.path.isfile(bundled_bin) and os.access(bundled_bin, os.X_OK):
        # Standalone binary — invoke directly, no Python interpreter prefix
        repo_candidates.append(([bundled_bin], f"bundled esptool binary: {bundled_bin}"))
    if has_pyserial and os.path.isfile(pio_esptool):
        repo_candidates.append(([sys.executable, pio_esptool], f"PlatformIO esptool: {pio_esptool}"))

    system_candidates = []
    if shutil.which("esptool.py"):
        system_candidates.append((["esptool.py"], "system esptool.py from PATH"))
    if shutil.which("esptool"):
        system_candidates.append((["esptool"], "system esptool from PATH"))
    for candidate in [
        os.path.expanduser("~/.local/bin/esptool"),
        os.path.expanduser("~/.local/bin/esptool.py"),
    ]:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            system_candidates.append(([candidate], f"user-local esptool: {candidate}"))

    search_order = system_candidates + repo_candidates if prefer_system else repo_candidates + system_candidates
    for command, source in search_order:
        print(f"  Found {source}")
        return command

    if not has_pyserial and os.path.isfile(pio_esptool):
        print("Found PlatformIO esptool.py but pyserial is not installed.")
        print("Install it with:  pip install pyserial")
        print("Or place the standalone esptool binary at: Release/esptool/esptool")
        sys.exit(1)

    return None


def esptool_supports_write_verify(esptool_cmd):
    """Return True if this esptool build accepts ``write_flash --verify``.

    esptool v5 removed ``--verify`` from write-flash, while older releases
    still accept it. Probe once and cache the result so flashing can choose
    the compatible verification path.
    """
    cache_key = tuple(esptool_cmd)
    if cache_key in _esptool_write_verify_support:
        return _esptool_write_verify_support[cache_key]

    try:
        result = subprocess.run(
            esptool_cmd + ["write_flash", "-h"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        output = (result.stdout or "") + (result.stderr or "")
        supported = "--verify" in output
    except Exception:
        supported = False

    _esptool_write_verify_support[cache_key] = supported
    return supported


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


def _archive_cache_path():
    """Return path to the cached firmware archive zip."""
    return os.path.join(_cache_dir(), FIRMWARE_ARCHIVE)


def _extracted_firmware_path(firmware_name):
    """Return path to an extracted firmware binary in the flat cache dir."""
    return os.path.join(_cache_dir(), firmware_name)


def _cache_meta_path():
    """Return path to the archive cache metadata JSON (single file for all variants)."""
    return os.path.join(_cache_dir(), "meta.json")


def _read_cache_meta():
    """Read archive cache metadata, returning dict or None if not cached."""
    import json
    path = _cache_meta_path()
    if os.path.isfile(path):
        try:
            with open(path) as f:
                return json.load(f)
        except Exception:
            pass
    return None


def _write_cache_meta(tag, sha256):
    """Write archive cache metadata after a successful download."""
    import json
    os.makedirs(_cache_dir(), exist_ok=True)
    with open(_cache_meta_path(), "w") as f:
        json.dump({"tag": tag, "sha256": sha256}, f, indent=2)


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


def fetch_firmware(board_key, flash_size, release_tag=None):
    """Fetch firmware from the GitHub release archive, using cache when possible.

    Downloads ``rtnode_firmware.zip`` once and extracts the correct binary for
    the given board/flash-size combination.  Falls back gracefully to the old
    per-board binary if the archive is not present in the release (backward compat).

    Returns (firmware_path, release_tag) on success, (None, reason) on failure.
    """
    import zipfile
    from urllib.request import urlretrieve

    # Resolve variant (fallback to smallest/safest if exact size missing)
    variants = BOARD_PROFILES[board_key]["flash_variants"]
    if flash_size not in variants:
        flash_size = sorted(variants, key=lambda s: int(s.replace("MB", "")))[0]
        print(f"  ⚠  No {flash_size} variant for {board_key} — using {flash_size}")
    variant = variants[flash_size]
    firmware_name = variant["firmware_bin"]
    extracted_path = _extracted_firmware_path(firmware_name)
    archive_path   = _archive_cache_path()
    cache_meta     = _read_cache_meta()

    # 1. Fetch release info
    label = f"release {release_tag}" if release_tag else "latest release"
    print(f"Checking {label} from {GITHUB_REPO}...")
    release, err = _fetch_release_info(release_tag)
    if not release:
        print(f"  Could not reach GitHub: {err}")
        if cache_meta and os.path.isfile(extracted_path):
            print(f"  Using cached firmware: {cache_meta.get('tag', '?')}")
            return extracted_path, cache_meta.get("tag", "cached")
        return None, f"No cached firmware and GitHub unreachable: {err}"

    remote_tag = release.get("tag_name", "unknown")

    # 2. Check whether cached archive is still valid
    need_download = True
    if cache_meta and os.path.isfile(archive_path):
        cached_tag = cache_meta.get("tag")
        if cached_tag == remote_tag:
            if sha256_file(archive_path) == cache_meta.get("sha256"):
                print(f"  Cached firmware archive is up-to-date: {remote_tag}")
                need_download = False
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

    if need_download:
        # 3. Locate the archive asset (with per-board fallback for old releases)
        asset_url   = None
        fallback_url = None
        fallback_name = None
        for asset in release.get("assets", []):
            if asset["name"] == FIRMWARE_ARCHIVE:
                asset_url = asset["browser_download_url"]
            if asset["name"] == firmware_name:
                fallback_url  = asset["browser_download_url"]
                fallback_name = asset["name"]

        os.makedirs(_cache_dir(), exist_ok=True)

        if asset_url:
            print(f"  Downloading {remote_tag} / {FIRMWARE_ARCHIVE}...")
            try:
                urlretrieve(asset_url, archive_path)
            except Exception as e:
                return None, f"Download failed: {e}"
            file_sha = sha256_file(archive_path)
            _write_cache_meta(remote_tag, file_sha)
            print(f"  Downloaded {os.path.getsize(archive_path):,} bytes  SHA-256: {file_sha[:16]}...")

        elif fallback_url:
            # Old-style release — download the individual binary directly
            print(f"  Archive '{FIRMWARE_ARCHIVE}' not in release — downloading {fallback_name}")
            try:
                urlretrieve(fallback_url, extracted_path)
            except Exception as e:
                return None, f"Download failed: {e}"
            file_sha = sha256_file(extracted_path)
            _write_cache_meta(remote_tag, file_sha)
            print(f"  Downloaded {os.path.getsize(extracted_path):,} bytes  SHA-256: {file_sha[:16]}...")
            return extracted_path, remote_tag

        else:
            available = [a["name"] for a in release.get("assets", [])]
            return None, (
                f"Neither '{FIRMWARE_ARCHIVE}' nor '{firmware_name}' found in release {remote_tag}.\n"
                f"  Available assets: {available}"
            )

    # 4. Extract the correct variant from the archive.
    #    Prefer the pre-merged binary — it is self-contained (bootloader + partitions
    #    + app in one file) so flash.py never needs PlatformIO or boot components.
    #    Fall back to the app-only binary for backward compat with old archives.
    if not os.path.isfile(archive_path):
        return None, f"Archive not found: {archive_path}"
    try:
        with zipfile.ZipFile(archive_path) as zf:
            names = zf.namelist()
            # Try merged binary first
            merged_name = variant.get("merged_bin")
            merged_path = _extracted_firmware_path(merged_name) if merged_name else None
            if merged_name and merged_name in names:
                with zf.open(merged_name) as src, open(merged_path, "wb") as dst:
                    dst.write(src.read())
                print(f"  Extracted {merged_name} (pre-merged, full-flash-ready)")
                return merged_path, remote_tag
            # Fall back to app-only binary
            if firmware_name not in names:
                return None, (
                    f"Neither '{merged_name or '?'}' nor '{firmware_name}' found in archive.\n"
                    f"  Archive contains: {names}"
                )
            with zf.open(firmware_name) as src, open(extracted_path, "wb") as dst:
                dst.write(src.read())
            print(f"  Extracted {firmware_name}")
    except Exception as e:
        return None, f"Failed to extract firmware from archive: {e}"

    return extracted_path, remote_tag


def _do_merge(output_path, esptool_cmd, bootloader, partitions, boot_app0, firmware):
    """Low-level merge: combine the four components into a single binary."""
    print("Merging firmware components...")
    print(f"  Bootloader: {bootloader}  @ 0x{BOOTLOADER_ADDR:04x}")
    print(f"  Partitions: {partitions}  @ 0x{PARTITIONS_ADDR:04x}")
    print(f"  boot_app0:  {boot_app0}   @ 0x{BOOT_APP0_ADDR:04x}")
    print(f"  Firmware:   {firmware}    @ 0x{APP_ADDR:05x}")

    flash_mode = BOARD_FLASH_MODE()
    print(f"  Flash mode: {flash_mode.upper()}")
    cmd = esptool_cmd + [
        "--chip", CHIP,
        "merge_bin",
        "--flash_mode", flash_mode,
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


def ensure_firmware_built():
    """Ensure PlatformIO firmware and boot components exist, building if needed.

    Returns True if all build artifacts are available (existing or freshly
    built), False if PlatformIO is not installed or the build fails.
    """
    firmware = FIRMWARE_BIN()
    if os.path.isfile(firmware):
        return True

    print(f"\n  Firmware not found: {firmware}")
    print(f"  Building with PlatformIO (env: {PIO_ENV()})...")

    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        print("  Error: PlatformIO not found. Install from https://platformio.org")
        return False

    result = subprocess.run([pio, "run", "-e", PIO_ENV()],
                            cwd=os.path.dirname(os.path.abspath(__file__)))
    if result.returncode != 0:
        print("  PlatformIO build failed.")
        return False

    if not os.path.isfile(firmware):
        print(f"  Error: Build succeeded but firmware not found: {firmware}")
        return False

    print("  Build complete.")
    return True


def merge_firmware(output_path, esptool_cmd):
    """Merge bootloader + partitions + boot_app0 + app into a single binary.

    Uses PlatformIO build output.  Triggers a build automatically if the
    firmware binary is not present.
    """
    ensure_firmware_built()

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

    Finds bootloader, partitions, and boot_app0 from PlatformIO build output.
    Triggers a PlatformIO build automatically if firmware is not yet built,
    since bootloader.bin and partitions.bin come from the same build.

    Returns the path to the merged binary on success, or None on failure.
    """
    ensure_firmware_built()

    bootloader = find_bootloader()
    partitions = find_partitions()
    boot_app0  = BOOT_APP0_BIN

    missing = []
    if not bootloader: missing.append("bootloader.bin")
    if not partitions: missing.append("partitions.bin")
    if not boot_app0:  missing.append("boot_app0.bin")

    if missing:
        print(f"Cannot auto-merge: missing {', '.join(missing)}")
        print(f"Build with PlatformIO first: pio run -e {PIO_ENV()}")
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
            False — partition table mismatch or unreadable state (device needs full flash)
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
        return False

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

    Returns True if app firmware is detected, False if blank/absent or unreadable.
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
            return False
        with open(tmp.name, "rb") as f:
            data = f.read()
        # All 0xFF means flash is blank — no app present
        if data == b'\xff' * len(data):
            return False
        return True
    except Exception as e:
        print(f"  Warning: App check failed: {e}")
        return False
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


def verify_firmware(firmware_path, port, esptool_cmd, baud=None,
                    flash_mode=None, no_hard_reset=False):
    """Verify flashed firmware using esptool's dedicated verify command."""
    if baud is None:
        baud = BAUD_RATE()
    flash_size = FLASH_SIZE()
    mode = flash_mode or BOARD_FLASH_MODE()

    is_merged = is_merged_binary(firmware_path)
    flash_addr = f"0x{BOOTLOADER_ADDR:x}" if is_merged else f"0x{APP_ADDR:x}"
    after_arg = "no_reset" if no_hard_reset else "hard_reset"

    print("\nVerifying flashed firmware...")
    cmd = esptool_cmd + [
        "--chip", CHIP,
        "--port", port,
        "--baud", baud,
        "--before", "no_reset",
        "--after", after_arg,
        "verify_flash",
        "--flash_mode", mode,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", flash_size,
        flash_addr, firmware_path,
    ]

    print("Running: " + " ".join(cmd[-8:]))
    result = subprocess.run(cmd)
    return result.returncode == 0


def flash_firmware(firmware_path, port, esptool_cmd, baud=None,
                   no_reset_before=False, verify=False,
                   flash_mode=None, no_hard_reset=False):
    """Flash firmware to the device.

    Args:
        no_reset_before: If True, use ``--before no_reset`` so we don't try to
            re-enter download mode (device is already in stub after erase).
        verify: If True, add ``--verify`` for read-back verification.
        flash_mode: Override flash mode (default: board profile).
        no_hard_reset: If True, use ``--after no_reset`` to keep device in stub.
    """
    if baud is None:
        baud = BAUD_RATE()
    flash_size = FLASH_SIZE()
    mode = flash_mode or BOARD_FLASH_MODE()
    print(f"\nFlashing {firmware_path} to {port}...")
    print(f"  Chip: {CHIP}  Baud: {baud}  Flash: {flash_size}  Mode: {mode.upper()}\n")

    # Determine if this is a merged binary (flash at 0x0) or app-only (flash at 0x10000)
    is_merged = is_merged_binary(firmware_path)

    if is_merged:
        flash_addr = f"0x{BOOTLOADER_ADDR:x}"
        print(f"  Detected: merged binary (partition table at 0x8000) -> flash at {flash_addr}")
    else:
        flash_addr = f"0x{APP_ADDR:x}"
        print(f"  Detected: app-only binary -> flash at {flash_addr}")

    inline_verify = verify and esptool_supports_write_verify(esptool_cmd)
    post_write_verify = verify and not inline_verify
    before_arg = "no_reset" if no_reset_before else "default_reset"
    after_arg  = "no_reset" if (no_hard_reset or post_write_verify) else "hard_reset"

    cmd = esptool_cmd + [
        "--chip", CHIP,
        "--port", port,
        "--baud", baud,
        "--before", before_arg,
        "--after", after_arg,
        "write_flash",
        "-z",
        "--flash_mode", mode,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", flash_size,
    ]
    if inline_verify:
        cmd.append("--verify")
    cmd += [flash_addr, firmware_path]

    print("Running: " + " ".join(cmd[-8:]))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        return False

    if post_write_verify:
        return verify_firmware(
            firmware_path,
            port,
            esptool_cmd,
            baud=baud,
            flash_mode=mode,
            no_hard_reset=no_hard_reset,
        )

    return True


def _monitor_boot(port, timeout=8):
    """Open serial port and watch for boot errors for `timeout` seconds.

    Returns:
        (True,  output)  — device appears to be booting normally
        (False, output)  — bootloop detected (ets_loader.c / repeated resets)
        (None,  reason)  — could not open serial port
    """
    try:
        import serial as pyserial
    except ImportError:
        return None, "pyserial not installed — skipping boot check"

    try:
        ser = pyserial.Serial(port, 115200, timeout=1)
    except Exception as e:
        return None, f"Could not open {port}: {e}"

    print(f"\n  Monitoring boot on {port} for {timeout}s...")
    output = ""
    reset_count = 0
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            raw = ser.read(ser.in_waiting or 1)
            if raw:
                text = raw.decode("utf-8", errors="replace")
                output += text
                # Count ROM reset lines — 2+ means bootloop
                reset_count += text.count("ets_loader.c")
                if reset_count >= 2:
                    ser.close()
                    return False, output
                # Any application output means boot succeeded
                if "[Boundary]" in output or "Node" in output or "WiFi" in output:
                    ser.close()
                    return True, output
    except Exception:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass

    # If we got reset output but only once, device may still be trying to boot
    if reset_count >= 1 and ("ets_loader.c" in output):
        return False, output

    # No clear signal — assume OK (normal if serial takes time)
    return True, output


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    global _board, _detected_flash_size
    parser = argparse.ArgumentParser(
        description="RTNode-HeltecV4 Flash Utility — flash transport node firmware to Heltec V3/V4",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python flash.py
      Download latest firmware and flash default board.
  python flash.py --update
      Legacy alias for an app-only update.
  python flash.py --use-system-esptool
      Prefer a host-installed esptool over the bundled Release copy.
  python flash.py --board v3
      Download latest firmware and flash a V3 board.
  python flash.py --release v1.0.12
      Flash a specific release tag.
  python flash.py --full
      Do a full flash with the merged binary.
  python flash.py --offline
      Use only cached or local firmware.
  python flash.py --file firmware.bin
      Flash a specific local binary.
  python flash.py --merge-only
      Build the merged release binary without flashing.
  python flash.py --port /dev/ttyACM0
      Use a specific serial port.
  python flash.py --erase
      Erase flash first, then do a full flash.
        """,
    )
    parser.add_argument("--board", choices=["v3", "v4", "tracker"], default=None,
                        help="Target board: v3 (Heltec V3), v4 (Heltec V4), or tracker (Heltec Wireless Tracker v1.1). "
                             "Auto-detected from connected device if omitted.")
    parser.add_argument("--file", "-f", help="Path to firmware binary to flash")
    parser.add_argument("--port", "-p", help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", "-b", default=None, help="Baud rate (board-specific default)")
    parser.add_argument("--release", "-r", default=None, metavar="TAG",
                        help="Flash a specific release version (e.g. v1.0.12)")
    parser.add_argument("--update", action="store_true",
                        help="Legacy alias for an app-only firmware update")
    parser.add_argument("--offline", action="store_true",
                        help="Skip online check — use cached or local firmware only")
    parser.add_argument("--merge-only", action="store_true",
                        help="Merge PlatformIO build output into single binary, don't flash")
    parser.add_argument("--full", action="store_true",
                        help="Flash merged binary (bootloader + partitions + app) — overwrites everything")
    parser.add_argument("--erase", action="store_true",
                        help="Erase entire flash before writing (implies --full)")
    parser.add_argument("--use-system-esptool", action="store_true",
                        help="Use a host-installed esptool instead of the bundled Release copy")
    # Power-user override (not shown in --help)
    parser.add_argument("--flash-mode", default=None,
                        help=argparse.SUPPRESS)

    args = parser.parse_args()

    if args.update and args.offline:
        parser.error("--update cannot be combined with --offline")

    if args.update:
        print("Using legacy compatibility flag; default behavior already downloads and flashes the latest firmware unless --offline is set.")

    # Find esptool early — needed for both auto-detect and flashing
    esptool_cmd = find_esptool(prefer_system=args.use_system_esptool)
    if not esptool_cmd:
        print("Error: esptool not found!")
        print("Expected one of:")
        print("  1. Bundled standalone binary: Release/esptool/esptool")
        print("  2. PlatformIO's packaged esptool (with pyserial installed)")
        print("  3. A host-installed esptool (pip install esptool)")
        sys.exit(1)

    # ── Board detection ─────────────────────────────────────────────────
    detected_info = None
    _early_port = None

    if args.board:
        # Explicit board — keep the selected profile, but still probe the device
        # when a port is available so flash size can override stale profile data.
        _board = args.board
        _early_port = args.port or find_serial_port()
        if _early_port:
            print(f"Reading flash info from {_early_port}...")
            info, err = read_flash_info(_early_port, esptool_cmd)
            if info:
                detected_info = info
                actual_flash = info.get("flash_size")
                if actual_flash:
                    _detected_flash_size = actual_flash
                print(f"  Chip:       {info.get('chip', '?')}")
                print(f"  Flash:      {actual_flash or '?'}")
                print(f"  Features:   {info.get('features', '?')}")
                print(f"  MAC:        {info.get('mac', '?')}")
            else:
                print(f"  Flash probe failed: {err}")
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
                actual_flash = info.get("flash_size")
                if actual_flash:
                    _detected_flash_size = actual_flash
                print(f"  Chip:       {info.get('chip', '?')}")
                print(f"  Flash:      {actual_flash or '?'}")
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
    print("║    RTNode-HeltecV4 Flash Utility         ║")
    print(f"║  {bp['name']:^40s}  ║")
    print("╚══════════════════════════════════════════╝")
    print()
    print(f"Using esptool: {' '.join(esptool_cmd)}")

    # --erase implies --full (after erase, device needs bootloader + partitions)
    if args.erase:
        args.full = True

    # Apply flash mode override (hidden --flash-mode flag for power users)
    global _flash_mode_override
    if args.flash_mode:
        _flash_mode_override = args.flash_mode

    fv       = flash_variant()
    print(f"  Flash size: {FLASH_SIZE()}"
          + (" (detected from device)" if _detected_flash_size else " (conservative default)"))
    print(f"  Variant:    {fv['firmware_bin']}")
    print(f"  Flash mode: {BOARD_FLASH_MODE().upper()}"
          + (" (override)" if _flash_mode_override else " (board default)"))

    # Determine firmware file
    firmware_path = None
    # Paths within the PlatformIO build output dir
    merged_fn    = MERGED_BIN() or os.path.join(fv["build_dir"],
                                                 fv["firmware_bin"].replace(".bin", "_merged.bin"))
    firmware_bin = FIRMWARE_BIN()
    pio_env      = PIO_ENV()
    # Merged binary name shown in the variant (used for cache lookup)
    merged_bin_name = fv.get("merged_bin")

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
        # Full flash, offline: prefer release-ZIP firmware next to flash.py,
        # then PIO build output, then firmware cache.
        local = find_local_firmware()
        if local:
            firmware_path = local
            print(f"Using local firmware: {local}")
        elif os.path.isfile(firmware_bin):
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
            # Try firmware cache (previously downloaded)
            cached_merged = _extracted_firmware_path(merged_bin_name) if merged_bin_name else None
            cached_app    = _extracted_firmware_path(fv["firmware_bin"])
            if cached_merged and os.path.isfile(cached_merged):
                firmware_path = cached_merged
                meta = _read_cache_meta()
                print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
            elif os.path.isfile(cached_app):
                firmware_path = cached_app
                meta = _read_cache_meta()
                print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
            else:
                print("No firmware found for full flash!")
                print()
                print("Options:")
                print(f"  1. Use firmware from the release ZIP (extract next to flash.py)")
                print(f"  2. Build with PlatformIO:  pio run -e {pio_env}")
                print(f"  3. Run without --offline to download from GitHub")
                print(f"  4. Specify a file:  python flash.py --board {_board} --file <path>")
                sys.exit(1)

    else:
        # Default path: fetch from GitHub (unless --offline)
        if not args.offline:
            fw_path, tag_or_err = fetch_firmware(_board, FLASH_SIZE(), release_tag=args.release)
            if fw_path:
                firmware_path = fw_path
                print(f"\n  Release: {tag_or_err}")
            else:
                print(f"\n  GitHub: {tag_or_err}")
                print("  Falling back to local firmware...")

        # Fall back to local firmware alongside flash.py, PIO build, or cache
        if not firmware_path:
            local = find_local_firmware()
            if local:
                firmware_path = local
                print(f"Using local firmware: {local}")
            elif os.path.isfile(firmware_bin):
                firmware_path = firmware_bin
                print(f"Using local PlatformIO build: {firmware_bin}")
            else:
                cached_merged = _extracted_firmware_path(merged_bin_name) if merged_bin_name else None
                cached_app    = _extracted_firmware_path(fv["firmware_bin"])
                if cached_merged and os.path.isfile(cached_merged):
                    firmware_path = cached_merged
                    meta = _read_cache_meta()
                    print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
                elif os.path.isfile(cached_app):
                    firmware_path = cached_app
                    meta = _read_cache_meta()
                    print(f"Using cached firmware: {meta.get('tag', '?') if meta else '?'}")
                elif os.path.isfile(merged_fn):
                    firmware_path = merged_fn
                    print(f"Using local merged binary: {merged_fn}")
                else:
                    print("No firmware found!")
                    print()
                    print("Options:")
                    print(f"  1. Use firmware from the release ZIP (extract next to flash.py)")
                    print(f"  2. Build with PlatformIO:  pio run -e {pio_env}")
                    print(f"  3. Specify a file:  python flash.py --board {_board} --file <path>")
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
        print(f"  WiFi/transport settings will be preserved")

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
    erase_performed = False
    if args.erase:
        print(f"\nErasing flash on {port}...")
        # Use --after no_reset so the device stays in the esptool stub after
        # erasing. This avoids exiting download mode (which would require
        # DTR/RTS re-entry and can fail on some USB-UART bridges).
        erase_cmd = esptool_cmd + [
            "--chip", CHIP,
            "--port", port,
            "--baud", baud,
            "--after", "no_reset",
            "erase_flash",
        ]
        result = subprocess.run(erase_cmd)
        if result.returncode != 0:
            print("\nErase FAILED.")
            sys.exit(1)
        erase_performed = True
        print("Flash erased (device still in download mode).")
        time.sleep(1)   # brief settle

    # ── Flash + auto-verify + boot-check + auto-retry ───────────────────────
    #
    # Strategy:
    #   1. Flash with the board's default flash mode
    #   2. If this is a full flash (any path), always add --verify
    #   3. After successful flash+verify, monitor serial for bootloop
    #   4. If bootloop detected and current mode != DIO, auto-retry with DIO
    #
    current_mode = BOARD_FLASH_MODE()

    ok = flash_firmware(firmware_path, port, esptool_cmd, baud,
                        no_reset_before=erase_performed,
                        verify=full_flash)

    if not ok:
        print("\nFlash FAILED. Check connection and try again.")
        print("You may need to hold BOOT while pressing RESET.")
        sys.exit(1)

    # ── Post-flash boot monitoring (on any full flash) ──────────────────────
    if full_flash:
        print("\n  Verifying device boots correctly...")
        time.sleep(2)  # Give device time to start booting
        boot_ok, boot_output = _monitor_boot(port, timeout=8)

        if boot_ok is None:
            # Couldn't open serial — not fatal, just warn
            print(f"  ⚠  {boot_output}")
            print("  Cannot verify boot — check device manually")
        elif boot_ok:
            print("  ✓ Device is booting normally")
        else:
            # Bootloop detected!
            print("\n  ✗ BOOTLOOP DETECTED — device is not booting properly")
            if boot_output:
                # Show the first few relevant lines
                for line in boot_output.splitlines()[:8]:
                    line = line.strip()
                    if line:
                        print(f"    {line}")

            if current_mode != "dio":
                print(f"\n  Current flash mode is {current_mode.upper()} — retrying with DIO...")
                print("  (DIO is more compatible with all flash chip variants)")

                # Need to re-enter download mode: reset via 1200 baud
                print("  Resetting device to download mode...")
                reset_to_bootloader(port)
                time.sleep(3)
                new_port = args.port or find_serial_port()
                if new_port:
                    port = new_port

                # Re-erase if we erased before (flash is garbage after bootloop)
                if args.erase:
                    print(f"\n  Re-erasing flash on {port}...")
                    erase_cmd = esptool_cmd + [
                        "--chip", CHIP,
                        "--port", port,
                        "--baud", baud,
                        "--after", "no_reset",
                        "erase_flash",
                    ]
                    result = subprocess.run(erase_cmd)
                    if result.returncode != 0:
                        print("\n  Re-erase FAILED.")
                        sys.exit(1)
                    erase_performed = True
                    time.sleep(1)

                ok = flash_firmware(firmware_path, port, esptool_cmd, baud,
                                    no_reset_before=erase_performed,
                                    verify=True, flash_mode="dio")

                if not ok:
                    print("\n  DIO retry FAILED.")
                    sys.exit(1)

                # Check boot again
                time.sleep(2)
                boot_ok2, boot_output2 = _monitor_boot(port, timeout=8)
                if boot_ok2 is False:
                    print("\n  ✗ Still bootlooping after DIO retry.")
                    print("  This may be a hardware issue. Check connections and try a different USB cable.")
                    sys.exit(1)
                elif boot_ok2:
                    print("  ✓ Device is booting normally with DIO mode!")
                else:
                    print(f"  ⚠  {boot_output2}")
                    print("  Could not verify boot — check device manually")
            else:
                print("\n  Already using DIO mode — this may be a hardware issue.")
                print("  Try: different USB cable, different port, or reflash the original firmware:")
                print(f"    python flash.py --erase --board {_board}")
                sys.exit(1)

    print()
    print("╔══════════════════════════════════════════╗")
    print("║          Flash complete!                 ║")
    print("║  Device will reboot automatically.       ║")
    print("║                                          ║")
    print("║  On first boot, hold PRG > 5s to enter   ║")
    print("║  the configuration portal.               ║")
    print("╚══════════════════════════════════════════╝")


if __name__ == "__main__":
    main()
