# RNodeTHV4 — Reticulum Boundary Node for Heltec WiFi LoRa 32 V4

A custom firmware for the **Heltec WiFi LoRa 32 V4** (ESP32-S3 + SX1262) that operates as a **Boundary Node** — bridging a local LoRa radio network with a remote TCP/IP backbone (such as [rmap.world](https://rmap.world)) over WiFi.

```
  Android / Sideband                                            Remote
  ┌──────────┐          ┌──────────────┐        WiFi         Reticulum
  │ Sideband │◄── BT ──►│ RNode (V4)   │◄── TCP ──────────► Backbone
  │   App    │          │ Boundary Mode│        ▲            (rnsd /
  └──────────┘          └──────┬───────┘        │            rmap.world)
                               │            ┌───┴───┐
                          LoRa Radio        │ Router │
                               │            └───────┘
                        ◄── RF mesh ──►
                         Other RNodes
```

Built on [microReticulum](https://github.com/attermann/microReticulum) (a C++ port of the [Reticulum](https://reticulum.network/) network stack) and the [RNode firmware](https://github.com/markqvist/RNode_Firmware) by Mark Qvist.

## Features

- **Bidirectional LoRa ↔ TCP bridging** — local LoRa mesh nodes can reach the global Reticulum backbone and vice versa
- **Web-based configuration portal** — WiFi SSID/password, backbone host/port, LoRa parameters, all configurable via captive portal
- **OLED status display** — real-time status indicators for LoRa, WiFi, WAN (backbone), LAN (local TCP), plus IP address, port, and airtime
- **Optional local TCP server** — serve local devices on your WiFi in addition to the backbone connection
- **Automatic reconnection** — WiFi and TCP connections recover from drops with exponential backoff
- **ESP32 memory-optimized** — table sizes, timeouts, and caching tuned for the constrained MCU environment

## Hardware

| Component | Spec |
|-----------|------|
| **Board** | Heltec WiFi LoRa 32 V4 |
| **MCU** | ESP32-S3, 2MB PSRAM, 16MB Flash |
| **Radio** | SX1262 + GC1109 PA (up to 28 dBm) |
| **Display** | SSD1306 OLED 128×64 |
| **WiFi** | 2.4 GHz 802.11 b/g/n |

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) installed (via VS Code extension or CLI)
- Heltec WiFi LoRa 32 V4 connected via USB

### Build & Flash

```bash
# Clone this repo
git clone https://github.com/jrl290/RNodeTHV4.git
cd RNodeTHV4

# Build
pio run -e heltec_V4_boundary

# Flash
pio run -e heltec_V4_boundary -t upload

# Monitor serial output (optional)
pio device monitor -e heltec_V4_boundary
```

On first boot (or if no configuration is found), the device automatically enters the **Configuration Portal**.

## Configuration Portal

### Entering Config Mode

The config portal activates automatically on:
- **First boot** — when no saved configuration exists
- **Button hold >5 seconds** — hold the PRG button for 5+ seconds, the device reboots into config mode

When active, the device creates a WiFi access point named **`RNode-Boundary-Setup`** (open network). Connect to it and browse to `http://192.168.4.1`.

### Config Page Options

The web form has four sections:

#### 📶 WiFi Network
| Field | Description |
|-------|-------------|
| **WiFi** | Enable/Disable (disable for LoRa-only repeater mode) |
| **SSID** | Your WiFi network name |
| **Password** | WiFi password |

#### 🌐 TCP Backbone
| Field | Description |
|-------|-------------|
| **Mode** | `Disabled` or `Client (connect to backbone)` |
| **Backbone Host** | IP address or hostname of backbone server (e.g. `rmap.world`) |
| **Backbone Port** | TCP port (default: `4242`) |

#### 📡 Local TCP Server (optional)
| Field | Description |
|-------|-------------|
| **Local TCP Server** | Enable/Disable — runs a TCP server on your WiFi for local Reticulum nodes to connect |
| **TCP Port** | Port to listen on (default: `4242`) |

#### 📻 LoRa Radio
| Field | Description |
|-------|-------------|
| **Frequency** | e.g. `867.200` MHz — must match your other RNodes |
| **Bandwidth** | 7.8 kHz – 500 kHz (typically `125 kHz`) |
| **Spreading Factor** | SF6 – SF12 (typically `SF7` for backbone, `SF10` for long range) |
| **Coding Rate** | 4/5 – 4/8 |
| **TX Power** | 2 – 22 dBm |

After saving, the device reboots with the new configuration applied.

## OLED Display Layout

The 128×64 OLED is split into two panels:

### Left Panel — Status Indicators (64×64)

```
 ● LORA          ← filled circle = radio online
 ○ wifi          ← unfilled circle = WiFi disconnected
 ● WAN           ← filled = backbone TCP connected
 ○ LAN           ← unfilled = no local TCP clients
 ────────────────
 Air:0.3%        ← current LoRa airtime
 ▓▓▓▓▓ |||||||   ← battery, signal quality
```

- **Filled circle (●)** = active/connected
- **Unfilled circle (○)** = inactive/disconnected
- Labels are UPPERCASE when active, lowercase when inactive (except LAN which is always uppercase)

### Right Panel — Device Info (64×64)

```
 ▓▓ RNodeTHV4 ▓▓  ← title bar (inverted)
 867.200MHz       ← LoRa frequency
 SF7 125k         ← spreading factor & bandwidth
 ────────────────  ← separator
 192.168.1.42     ← WiFi IP address (or "No WiFi")
 Port:4242        ← backbone TCP port
 ────────────────  ← separator
```

## Interface Modes

The firmware runs **two RNS interfaces** simultaneously, using different interface modes to control announce propagation and routing behavior:

### LoRa Interface — `MODE_ACCESS_POINT`

The LoRa radio operates in **Access Point mode**. In Reticulum, this means:
- The interface broadcasts its own announces but **blocks rebroadcast of remote announces** from crossing to LoRa
- This prevents backbone announces (hundreds of remote destinations) from flooding the limited-bandwidth LoRa channel
- Local nodes discover the boundary node directly; the boundary node answers path requests for remote destinations from its cache

### TCP Backbone Interface — `MODE_BOUNDARY`

The TCP backbone connection uses a custom **Boundary mode** (`0x20`), a new interface mode added to microReticulum for this firmware. Boundary mode means:
- Incoming announces from the backbone are received and cached, but **not stored in the path table by default** — only stored when specifically requested via a path request from a local LoRa node
- This prevents the path table (limited to 48 entries on ESP32) from being overwhelmed by thousands of backbone destinations
- When the path table needs to be culled, **Boundary-mode paths are evicted first**, preserving locally-needed LoRa paths

### Optional Local TCP Server — `MODE_ACCESS_POINT`

If enabled, a TCP server on the WiFi network allows local Reticulum nodes to connect. It also uses Access Point mode, with the same announce filtering as LoRa.

## Routing & Memory Customizations

The ESP32-S3 has limited RAM compared to a desktop Reticulum node. Several customizations were made to the microReticulum library to operate reliably within these constraints:

### Table Size Limits

| Table | Default (Desktop) | RNodeTHV4 | Rationale |
|-------|-------------------|-----------|-----------|
| Path table (`_destination_table`) | Unbounded | **48 entries** | Prevents unbounded growth; boundary paths evicted first |
| Hash list (`_hashlist`) | 1,000,000 | **32** | Packet dedup list; small is fine for low-throughput LoRa |
| Path request tags (`_max_pr_tags`) | 32,000 | **32** | Pending path requests rarely exceed a few dozen |
| Known destinations | 100 | **24** | Identity cache; rarely need more on a boundary node |
| Max queued announces | 16 | **4** | Outbound announce queue; LoRa is slow, no point queuing many |
| Max receipts | 1,024 | **20** | Packet receipt tracking |

### Timeout Reductions

| Setting | Default | RNodeTHV4 | Rationale |
|---------|---------|-----------|-----------|
| Destination timeout | 7 days | **1 day** | Free memory faster; stale paths re-resolve automatically |
| Pathfinder expiry | 7 days | **1 day** | Same as above |
| AP path time | 24 hours | **6 hours** | AP paths go stale faster in mesh environments |
| Roaming path time | 6 hours | **1 hour** | Mobile nodes change paths frequently |
| Table cull interval | 5 seconds | **60 seconds** | Less CPU overhead on culling |
| Job/Clean/Persist intervals | 5m/15m/12h | **60s/60s/60s** | More frequent housekeeping for MCU stability |

### Selective Backbone Caching

The most critical optimization: **backbone announces are not stored in the path table by default**. A backbone like `rmap.world` may advertise hundreds of destinations. Storing them all would evict every local LoRa path.

Instead:
1. Backbone announces are received and their packets cached to flash storage
2. When a local LoRa node requests a path, the boundary checks its cache and responds directly
3. Only **specifically requested** paths get a path table entry
4. Path table culling prioritizes evicting backbone entries over local ones

### Default Route Forwarding

When a transport-addressed packet arrives from LoRa but the boundary has no path table entry for it, the firmware:
1. Strips the transport headers (converts `HEADER_2` → `HEADER_1/BROADCAST`)
2. Forwards the raw packet to the backbone interface
3. Creates reverse-table entries so proofs can route back to the sender

This acts as a **default route** — any packet the boundary can't route locally gets forwarded to the backbone.

### Cached Packet Unpacking Fix

The original microReticulum `get_cached_packet()` function called `update_hash()` after deserializing cached packets from flash. However, `update_hash()` only computes the packet hash — it does **not** parse the raw bytes into fields like `destination_hash`, `data`, `flags`, etc.

This was changed to call `unpack()` instead, which parses all packet fields AND computes the hash. Without this fix, path responses contained empty destination hashes and were silently dropped by LoRa nodes.

## Connecting to the Backbone

### Example: Connect to rmap.world

In the configuration portal:
1. Set WiFi SSID and password
2. Set TCP Backbone Mode to **Client**
3. Set Backbone Host to `rmap.world`
4. Set Backbone Port to `4242`
5. Save and reboot

### Example: Local rnsd Server

On your server, configure `rnsd` with a TCP Server Interface in `~/.reticulum/config`:

```ini
[interfaces]
  [[TCP Server Interface]]
    type = TCPServerInterface
    listen_host = 0.0.0.0
    listen_port = 4242
```

Then configure the boundary node as a **Client** pointing to your server's IP.

### Example: rnsd Connects to Boundary

On your server, configure `rnsd` with a TCP Client Interface:

```ini
[interfaces]
  [[TCP Client to Boundary]]
    type = TCPClientInterface
    target_host = <boundary-node-ip>
    target_port = 4242
```

Set the boundary node's **Local TCP Server** to **Enabled** (port 4242).

## Architecture

### Key Files

| File | Purpose |
|------|---------|
| `RNode_Firmware.ino` | Main firmware — boundary mode initialization, interface setup, button handling |
| `BoundaryMode.h` | Boundary state struct, EEPROM load/save, configuration defaults |
| `BoundaryConfig.h` | Web-based captive portal for configuration |
| `TcpInterface.h` | TCP backbone interface (implements `RNS::InterfaceImpl`) with HDLC framing |
| `Display.h` | OLED display layout — boundary-specific status page |
| `Boards.h` | Board variant definition for `heltec32v4_boundary` |
| `platformio.ini` | Build targets: `heltec_V4_boundary` and `heltec_V4_boundary-local` |

### Library Patches

The firmware depends on [microReticulum](https://github.com/attermann/microReticulum) `0.2.4`, automatically fetched by PlatformIO on first build. After the first build, the library sources under `.pio/libdeps/heltec_V4_boundary/microReticulum/src/` need the patches described in "Routing & Memory Customizations" above. Key files modified:

| File | Changes |
|------|---------|
| `Transport.cpp` | Selective caching, default route forwarding, boundary-aware culling, `get_cached_packet()` unpack fix, memory limits |
| `Transport.h` | `MODE_BOUNDARY`, `PacketEntry`, `Callbacks`, `cull_path_table()`, configurable table sizes |
| `Identity.cpp` | `_known_destinations_maxsize` = 24, `cull_known_destinations()` |
| `Type.h` | `MODE_BOUNDARY` = 0x20, reduced `MAX_QUEUED_ANNOUNCES`, `MAX_RECEIPTS`, shorter timeouts |

### Memory Usage (typical)

| Resource | Used | Available |
|----------|------|-----------|
| RAM | ~21.7% | 320 KB |
| Flash | ~18.1% | 16 MB |
| PSRAM | Dynamic | 2 MB |

## License

This project is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE) for details.

Based on:
- [RNode Firmware](https://github.com/markqvist/RNode_Firmware) by Mark Qvist (GPL-3.0)
- [microReticulum](https://github.com/attermann/microReticulum) by Chris Attermann (GPL-3.0)
- [Reticulum](https://reticulum.network/) by Mark Qvist (MIT)

