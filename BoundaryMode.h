// Copyright (C) 2026, Boundary Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// BoundaryMode.h — Configuration and runtime state for the Boundary Mode
// firmware variant. This header defines the WiFi backbone connection
// parameters and boundary-specific operational settings.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef BOUNDARY_MODE_H
#define BOUNDARY_MODE_H

#ifdef BOUNDARY_MODE

// ─── Boundary Mode Configuration ────────────────────────────────────────────
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
// Total: 0xD6 (214 bytes used of 256 CONFIG area)

#define BOUNDARY_ENABLE_BYTE 0x73

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

    EEPROM.commit();
}

#endif // BOUNDARY_MODE
#endif // BOUNDARY_MODE_H
