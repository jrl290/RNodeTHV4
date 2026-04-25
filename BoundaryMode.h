// Copyright (C) 2026, Boundary Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// BoundaryMode.h — Configuration and runtime state for the legacy
// "Boundary Mode" firmware variant. Going forward this should be renamed
// "Transport Mode". It is the only intended operating mode for this fork;
// the old multi-mode split is kept only for compatibility while the codebase
// is being simplified.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef BOUNDARY_MODE_H
#define BOUNDARY_MODE_H

#ifdef BOUNDARY_MODE

// Compatibility alias for the planned rename from Boundary Mode to
// Transport Mode. New code should prefer the transport terminology even
// while the legacy BOUNDARY_MODE compile-time flag still exists.
#ifndef TRANSPORT_MODE
#define TRANSPORT_MODE 1
#endif

// ─── Boundary Mode Configuration ────────────────────────────────────────────
//
// NOTE: "Boundary Mode" is the legacy name. This should be relabeled
// "Transport Mode" once the remaining non-transport code paths are removed.
// In practice this is the only supported mode in this firmware branch.
//
// The boundary node operates with TWO RNS interfaces:
//
//   1. LoRaInterface (MODE_GATEWAY) — radio side, handles LoRa mesh
//   2. BackboneInterface (MODE_BOUNDARY) — WiFi side, connects to TCP backbone
//
// RNS Transport is ALWAYS enabled in boundary mode.
// Packets received on either interface are routed through Transport
// to the other interface based on path table lookups and announce rules.

// ─── WiFi Backbone Connection ────────────────────────────────────────────────
// These can be overridden via build flags or EEPROM at runtime.

// Default backbone server to connect to (client mode)
// Set to empty string "" if operating in server mode
#ifndef BOUNDARY_BACKBONE_HOST
#define BOUNDARY_BACKBONE_HOST ""
#endif

#ifndef BOUNDARY_BACKBONE_PORT
#define BOUNDARY_BACKBONE_PORT 4242
#endif

// TCP interface mode: 0 = disabled, 1 = client (connect out)
#ifndef BOUNDARY_TCP_MODE
#define BOUNDARY_TCP_MODE 1
#endif

// TCP server listen port (when in server mode)
#ifndef BOUNDARY_TCP_PORT
#define BOUNDARY_TCP_PORT 4242
#endif

// ─── EEPROM Extension Addresses ──────────────────────────────────────────────
// We use the CONFIG area (config_addr) for additional boundary mode settings.
// These are after the existing WiFi SSID/PSK/IP/NM fields.
// Existing layout:
//   0x00-0x20: SSID (33 bytes)
//   0x21-0x41: PSK (33 bytes)
//   0x42-0x45: IP (4 bytes)
//   0x46-0x49: NM (4 bytes)
// Our additions (config_addr space, 0x4A onwards):
#define ADDR_CONF_BMODE      0x4A  // Boundary mode enabled flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_BTCP_MODE  0x4B  // TCP mode: 0=server, 1=client (1 byte)
#define ADDR_CONF_BTCP_PORT  0x4C  // TCP port (2 bytes, big-endian)
#define ADDR_CONF_BHOST      0x4E  // Backbone host (64 bytes, null-terminated)
#define ADDR_CONF_BHPORT     0x8E  // Backbone target port (2 bytes, big-endian)
#define ADDR_CONF_AP_TCP_EN  0x90  // AP TCP server enable (1 byte, 0x73 = enabled)
#define ADDR_CONF_AP_TCP_PORT 0x91 // AP TCP server port (2 bytes, big-endian)
#define ADDR_CONF_AP_SSID    0x93  // AP SSID (33 bytes, null-terminated)
#define ADDR_CONF_AP_PSK     0xB4  // AP PSK (33 bytes, null-terminated)
#define ADDR_CONF_WIFI_EN   0xD5  // WiFi enable flag (1 byte, 0x73 = enabled)
// IFAC (Interface Access Code) settings for LoRa interface
#define ADDR_CONF_IFAC_EN   0xD6  // IFAC enable flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_IFAC_NAME 0xD7  // Network name (33 bytes, null-terminated)
#define ADDR_CONF_IFAC_PASS 0xF8  // Passphrase (33 bytes, null-terminated)
#define ADDR_CONF_APP_MARKER0 0x119 // RTNode app marker byte 0
#define ADDR_CONF_APP_MARKER1 0x11A // RTNode app marker byte 1
#define ADDR_CONF_APP_VERSION 0x11B // RTNode app config version
// Device advertisement settings (advertise this node's parameters and
// optional GPS coordinates to the Reticulum network so external maps such
// as rmap.world can pin it). Stored after the app version markers so the
// existing layout is preserved and old saves keep working (uninitialised
// 0xFF bytes are interpreted as "advertisement disabled, no coordinates").
#define ADDR_CONF_ADVERT_EN     0x11C // Advertise enable flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_ADVERT_LAT    0x11D // Latitude as IEEE-754 double (8 bytes, host byte order)
#define ADDR_CONF_ADVERT_LON    0x125 // Longitude as IEEE-754 double (8 bytes, host byte order)
#define ADDR_CONF_ADVERT_JITTER 0x12D // Randomize ~0.5 km offset flag (1 byte, 0x73 = enabled)
// Total: 0x12E (302 bytes — extends beyond 256-byte CONFIG area into
//         unused EEPROM gap; safe on ESP32 where EEPROM starts at 824)

#define BOUNDARY_ENABLE_BYTE 0x73
#define BOUNDARY_APP_MARKER0 0x52
#define BOUNDARY_APP_MARKER1 0x54
#define BOUNDARY_APP_VERSION 0x01

// ─── Boundary Mode Runtime State ─────────────────────────────────────────────
#ifndef BOUNDARY_STATE_DEFINED
#define BOUNDARY_STATE_DEFINED
struct BoundaryState {
    bool     enabled;
    bool     wifi_enabled;    // false = LoRa-only repeater (no WiFi)
    uint8_t  tcp_mode;        // 0=disabled, 1=client
    uint16_t tcp_port;        // Local port (client outbound)
    char     backbone_host[64];
    uint16_t backbone_port;   // Target port for client mode

    // AP TCP server settings
    bool     ap_tcp_enabled;  // Whether to run a WiFi AP with TCP server
    uint16_t ap_tcp_port;     // Port for the AP TCP server
    char     ap_ssid[33];     // AP SSID
    char     ap_psk[33];      // AP PSK (empty = open)

    // IFAC settings for LoRa interface
    bool     ifac_enabled;    // Whether IFAC is configured
    char     ifac_netname[33]; // Network name (empty = not set)
    char     ifac_passphrase[33]; // Passphrase (empty = not set)

    // Device advertisement settings (used to advertise this node's
    // parameters and optional GPS location so external maps such as
    // rmap.world can pin this node). Defaults to disabled.
    bool     advert_enabled;  // Whether to advertise this device
    double   advert_lat;      // Latitude in decimal degrees (-90..90)
    double   advert_lon;      // Longitude in decimal degrees (-180..180)
    bool     advert_jitter;   // Randomize ~0.5 km offset for advertised coords

    // Runtime state
    bool     wifi_connected;
    bool     tcp_connected;       // Backbone (WAN) connected
    bool     ap_tcp_connected;    // Local TCP server (LAN) has client
    bool     ap_active;
    uint32_t packets_bridged_lora_to_tcp;
    uint32_t packets_bridged_tcp_to_lora;
    uint32_t last_bridge_activity;
};
#endif // BOUNDARY_STATE_DEFINED

// Global boundary state instance (defined in RNode_Firmware.ino)
extern BoundaryState boundary_state;

// ─── Boundary Mode EEPROM Load/Save ─────────────────────────────────────────

inline bool boundary_app_marker_valid() {
    return EEPROM.read(config_addr(ADDR_CONF_APP_MARKER0)) == BOUNDARY_APP_MARKER0 &&
           EEPROM.read(config_addr(ADDR_CONF_APP_MARKER1)) == BOUNDARY_APP_MARKER1;
}

inline bool boundary_app_version_matches() {
    return boundary_app_marker_valid() &&
           EEPROM.read(config_addr(ADDR_CONF_APP_VERSION)) == BOUNDARY_APP_VERSION;
}

inline void boundary_clear_app_marker() {
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER0), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER1), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_APP_VERSION), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BMODE), 0xFF);
    EEPROM.commit();
}

// ─── Helpers for serialising IEEE-754 doubles to EEPROM ──────────────────────
// Stored as 8 raw bytes in EEPROM order. If all 8 bytes read back as 0xFF
// (uninitialised), the value is treated as "not set" and a default is used.
inline void boundary_write_double(int addr, double value) {
    uint8_t buf[8];
    memcpy(buf, &value, sizeof(buf));
    for (int i = 0; i < 8; i++) {
        EEPROM.write(config_addr(addr + i), buf[i]);
    }
}

inline bool boundary_read_double(int addr, double& out) {
    uint8_t buf[8];
    bool all_ff = true;
    for (int i = 0; i < 8; i++) {
        buf[i] = EEPROM.read(config_addr(addr + i));
        if (buf[i] != 0xFF) all_ff = false;
    }
    if (all_ff) return false;
    memcpy(&out, buf, sizeof(out));
    // Reject NaN / inf values that may slip in from corrupted EEPROM.
    if (!(out == out) || out > 1e9 || out < -1e9) return false;
    return true;
}

inline void boundary_load_config() {
    // Check if boundary mode is configured
    uint8_t bmode = EEPROM.read(config_addr(ADDR_CONF_BMODE));
    boundary_state.enabled = (bmode == BOUNDARY_ENABLE_BYTE);

    if (!boundary_state.enabled) {
        // Use compile-time defaults
        boundary_state.wifi_enabled = true;
        boundary_state.tcp_mode = BOUNDARY_TCP_MODE;
        boundary_state.tcp_port = BOUNDARY_TCP_PORT;
        strncpy(boundary_state.backbone_host, BOUNDARY_BACKBONE_HOST,
                sizeof(boundary_state.backbone_host) - 1);
        boundary_state.backbone_host[sizeof(boundary_state.backbone_host) - 1] = '\0';
        boundary_state.backbone_port = BOUNDARY_BACKBONE_PORT;
        boundary_state.ap_tcp_enabled = false;
        boundary_state.ap_tcp_port = 4242;
        boundary_state.ap_ssid[0] = '\0';
        boundary_state.ap_psk[0] = '\0';
        boundary_state.ifac_enabled = false;
        boundary_state.ifac_netname[0] = '\0';
        boundary_state.ifac_passphrase[0] = '\0';
        boundary_state.advert_enabled = false;
        boundary_state.advert_lat = 0.0;
        boundary_state.advert_lon = 0.0;
        boundary_state.advert_jitter = false;
        // Mark as enabled since we're compiled with BOUNDARY_MODE
        boundary_state.enabled = true;
        return;
    }

    // Load wifi enable flag (default to enabled if unprogrammed 0xFF)
    uint8_t wifi_en_byte = EEPROM.read(config_addr(ADDR_CONF_WIFI_EN));
    boundary_state.wifi_enabled = (wifi_en_byte == BOUNDARY_ENABLE_BYTE || wifi_en_byte == 0xFF);

    // Load from EEPROM
    boundary_state.tcp_mode = EEPROM.read(config_addr(ADDR_CONF_BTCP_MODE));
    if (boundary_state.tcp_mode > 1) boundary_state.tcp_mode = 0; // 0=disabled, 1=client

    boundary_state.tcp_port =
        ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_BTCP_PORT)) << 8) |
        (uint16_t)EEPROM.read(config_addr(ADDR_CONF_BTCP_PORT + 1));
    if (boundary_state.tcp_port == 0 || boundary_state.tcp_port == 0xFFFF) {
        boundary_state.tcp_port = BOUNDARY_TCP_PORT;
    }

    for (int i = 0; i < 63; i++) {
        boundary_state.backbone_host[i] = EEPROM.read(config_addr(ADDR_CONF_BHOST + i));
        if (boundary_state.backbone_host[i] == 0xFF) {
            boundary_state.backbone_host[i] = '\0';
        }
    }
    boundary_state.backbone_host[63] = '\0';

    boundary_state.backbone_port =
        ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_BHPORT)) << 8) |
        (uint16_t)EEPROM.read(config_addr(ADDR_CONF_BHPORT + 1));
    if (boundary_state.backbone_port == 0 || boundary_state.backbone_port == 0xFFFF) {
        boundary_state.backbone_port = BOUNDARY_BACKBONE_PORT;
    }

    // Load AP TCP server settings
    boundary_state.ap_tcp_enabled =
        (EEPROM.read(config_addr(ADDR_CONF_AP_TCP_EN)) == BOUNDARY_ENABLE_BYTE);

    boundary_state.ap_tcp_port =
        ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_AP_TCP_PORT)) << 8) |
        (uint16_t)EEPROM.read(config_addr(ADDR_CONF_AP_TCP_PORT + 1));
    if (boundary_state.ap_tcp_port == 0 || boundary_state.ap_tcp_port == 0xFFFF) {
        boundary_state.ap_tcp_port = 4242;
    }

    for (int i = 0; i < 32; i++) {
        boundary_state.ap_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_AP_SSID + i));
        if (boundary_state.ap_ssid[i] == (char)0xFF) boundary_state.ap_ssid[i] = '\0';
    }
    boundary_state.ap_ssid[32] = '\0';

    for (int i = 0; i < 32; i++) {
        boundary_state.ap_psk[i] = EEPROM.read(config_addr(ADDR_CONF_AP_PSK + i));
        if (boundary_state.ap_psk[i] == (char)0xFF) boundary_state.ap_psk[i] = '\0';
    }
    boundary_state.ap_psk[32] = '\0';

    // Load IFAC settings
    boundary_state.ifac_enabled =
        (EEPROM.read(config_addr(ADDR_CONF_IFAC_EN)) == BOUNDARY_ENABLE_BYTE);

    for (int i = 0; i < 32; i++) {
        boundary_state.ifac_netname[i] = EEPROM.read(config_addr(ADDR_CONF_IFAC_NAME + i));
        if (boundary_state.ifac_netname[i] == (char)0xFF) boundary_state.ifac_netname[i] = '\0';
    }
    boundary_state.ifac_netname[32] = '\0';

    for (int i = 0; i < 32; i++) {
        boundary_state.ifac_passphrase[i] = EEPROM.read(config_addr(ADDR_CONF_IFAC_PASS + i));
        if (boundary_state.ifac_passphrase[i] == (char)0xFF) boundary_state.ifac_passphrase[i] = '\0';
    }
    boundary_state.ifac_passphrase[32] = '\0';

    // Load device advertisement settings. Defaults to disabled for both
    // the master toggle and the location jitter, so previously saved
    // configurations (which have 0xFF in these slots) keep advertising
    // off until the user explicitly enables it from the portal.
    {
        uint8_t advert_en_byte = EEPROM.read(config_addr(ADDR_CONF_ADVERT_EN));
        boundary_state.advert_enabled = (advert_en_byte == BOUNDARY_ENABLE_BYTE);

        if (!boundary_read_double(ADDR_CONF_ADVERT_LAT, boundary_state.advert_lat)) {
            boundary_state.advert_lat = 0.0;
        }
        if (!boundary_read_double(ADDR_CONF_ADVERT_LON, boundary_state.advert_lon)) {
            boundary_state.advert_lon = 0.0;
        }

        // Clamp to valid ranges in case of corrupted EEPROM data.
        if (boundary_state.advert_lat < -90.0 || boundary_state.advert_lat > 90.0) {
            boundary_state.advert_lat = 0.0;
        }
        if (boundary_state.advert_lon < -180.0 || boundary_state.advert_lon > 180.0) {
            boundary_state.advert_lon = 0.0;
        }

        uint8_t advert_jitter_byte = EEPROM.read(config_addr(ADDR_CONF_ADVERT_JITTER));
        boundary_state.advert_jitter = (advert_jitter_byte == BOUNDARY_ENABLE_BYTE);
    }

    // Reset runtime state
    boundary_state.packets_bridged_lora_to_tcp = 0;
    boundary_state.packets_bridged_tcp_to_lora = 0;
    boundary_state.last_bridge_activity = 0;
    boundary_state.wifi_connected = false;
    boundary_state.tcp_connected = false;
    boundary_state.ap_active = false;
}

inline void boundary_save_config() {
    EEPROM.write(config_addr(ADDR_CONF_BMODE), BOUNDARY_ENABLE_BYTE);
    EEPROM.write(config_addr(ADDR_CONF_WIFI_EN),
                 boundary_state.wifi_enabled ? BOUNDARY_ENABLE_BYTE : 0x00);
    EEPROM.write(config_addr(ADDR_CONF_BTCP_MODE), boundary_state.tcp_mode);
    EEPROM.write(config_addr(ADDR_CONF_BTCP_PORT), (boundary_state.tcp_port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BTCP_PORT + 1), boundary_state.tcp_port & 0xFF);
    for (int i = 0; i < 63; i++) {
        EEPROM.write(config_addr(ADDR_CONF_BHOST + i), boundary_state.backbone_host[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_BHOST + 63), 0x00);
    EEPROM.write(config_addr(ADDR_CONF_BHPORT), (boundary_state.backbone_port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BHPORT + 1), boundary_state.backbone_port & 0xFF);

    // AP TCP server settings
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_EN),
                 boundary_state.ap_tcp_enabled ? BOUNDARY_ENABLE_BYTE : 0x00);
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_PORT), (boundary_state.ap_tcp_port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_PORT + 1), boundary_state.ap_tcp_port & 0xFF);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_AP_SSID + i), boundary_state.ap_ssid[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_AP_SSID + 32), 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_AP_PSK + i), boundary_state.ap_psk[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_AP_PSK + 32), 0x00);

    // IFAC settings
    EEPROM.write(config_addr(ADDR_CONF_IFAC_EN),
                 boundary_state.ifac_enabled ? BOUNDARY_ENABLE_BYTE : 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IFAC_NAME + i), boundary_state.ifac_netname[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_IFAC_NAME + 32), 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IFAC_PASS + i), boundary_state.ifac_passphrase[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_IFAC_PASS + 32), 0x00);

    // Device advertisement settings
    EEPROM.write(config_addr(ADDR_CONF_ADVERT_EN),
                 boundary_state.advert_enabled ? BOUNDARY_ENABLE_BYTE : 0x00);
    boundary_write_double(ADDR_CONF_ADVERT_LAT, boundary_state.advert_lat);
    boundary_write_double(ADDR_CONF_ADVERT_LON, boundary_state.advert_lon);
    EEPROM.write(config_addr(ADDR_CONF_ADVERT_JITTER),
                 boundary_state.advert_jitter ? BOUNDARY_ENABLE_BYTE : 0x00);

    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER0), BOUNDARY_APP_MARKER0);
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER1), BOUNDARY_APP_MARKER1);
    EEPROM.write(config_addr(ADDR_CONF_APP_VERSION), BOUNDARY_APP_VERSION);

    EEPROM.commit();
}

#endif // BOUNDARY_MODE
#endif // BOUNDARY_MODE_H
