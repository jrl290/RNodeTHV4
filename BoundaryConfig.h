// Copyright (C) 2026, Boundary Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// BoundaryConfig.h — Captive-portal web configuration for the legacy
// "Boundary Mode" path. This should be renamed to "Transport Mode"
// together with the rest of the boundary-mode terminology. In this fork,
// transport/boundary mode is the only intended mode of operation.
// When triggered (first boot with no config, or button hold >5s),
// the device starts a WiFi AP with a web form for all settings:
//   WiFi STA credentials, TCP backbone params, LoRa radio params,
//   and optional AP-mode TCP server.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef BOUNDARY_CONFIG_H
#define BOUNDARY_CONFIG_H

#ifdef BOUNDARY_MODE

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ─── Node hash (cached in RTC by normal boot, read here without starting RNS) ─
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];

// ─── Config Portal State ─────────────────────────────────────────────────────
static bool config_portal_active = false;
static WebServer* config_server = nullptr;
static DNSServer* config_dns    = nullptr;

static const char CONFIG_AP_SSID[] = "RNode-Boundary-Setup";
static const uint16_t DNS_PORT = 53;
static const uint16_t HTTP_PORT = 80;

// Forward declarations
void config_portal_start();
void config_portal_stop();
void config_portal_loop();
bool config_portal_is_active();
bool boundary_needs_config();

// ─── Common bandwidth values (Hz) ───────────────────────────────────────────
// These match Reticulum standard channel plans
// Stored in flash (PROGMEM) to save ~200 bytes of RAM
static const uint32_t BW_OPTIONS_HZ[] PROGMEM = {
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000,
};
static const char BW_LABEL_0[]  PROGMEM = "7.8 kHz";
static const char BW_LABEL_1[]  PROGMEM = "10.4 kHz";
static const char BW_LABEL_2[]  PROGMEM = "15.6 kHz";
static const char BW_LABEL_3[]  PROGMEM = "20.8 kHz";
static const char BW_LABEL_4[]  PROGMEM = "31.25 kHz";
static const char BW_LABEL_5[]  PROGMEM = "41.7 kHz";
static const char BW_LABEL_6[]  PROGMEM = "62.5 kHz";
static const char BW_LABEL_7[]  PROGMEM = "125 kHz";
static const char BW_LABEL_8[]  PROGMEM = "250 kHz";
static const char BW_LABEL_9[]  PROGMEM = "500 kHz";
static const char* const BW_OPTIONS_LABELS[] PROGMEM = {
    BW_LABEL_0, BW_LABEL_1, BW_LABEL_2, BW_LABEL_3, BW_LABEL_4,
    BW_LABEL_5, BW_LABEL_6, BW_LABEL_7, BW_LABEL_8, BW_LABEL_9,
};
static const int BW_OPTIONS_COUNT = sizeof(BW_OPTIONS_HZ) / sizeof(BW_OPTIONS_HZ[0]);

static uint8_t config_default_display_rotation() {
    #if BOARD_MODEL == BOARD_LORA32_V2_1 || BOARD_MODEL == BOARD_TBEAM || BOARD_MODEL == BOARD_RAK4631
    return 0;
    #elif BOARD_MODEL == BOARD_HELTEC32_V2 || BOARD_MODEL == BOARD_HELTEC32_V3 || BOARD_MODEL == BOARD_HELTEC32_V4 || BOARD_MODEL == BOARD_HELTEC_T114 || BOARD_MODEL == BOARD_TBEAM_S_V1
    return 1;
    #else
    return 3;
    #endif
}

// ─── HTML Page Generation ────────────────────────────────────────────────────

static void config_send_html() {
    // Read current values from EEPROM/globals for pre-population
    char cur_ssid[33] = "";
    char cur_psk[33]  = "";

    for (int i = 0; i < 32; i++) {
        cur_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        if (cur_ssid[i] == (char)0xFF) cur_ssid[i] = '\0';
    }
    cur_ssid[32] = '\0';

    for (int i = 0; i < 32; i++) {
        cur_psk[i] = EEPROM.read(config_addr(ADDR_CONF_PSK + i));
        if (cur_psk[i] == (char)0xFF) cur_psk[i] = '\0';
    }
    cur_psk[32] = '\0';

    // Current LoRa values (from globals, which were loaded from EEPROM)
    uint32_t cur_freq = lora_freq;
    uint32_t cur_bw   = lora_bw;
    int cur_sf        = lora_sf;
    int cur_cr        = lora_cr;
    int cur_txp       = lora_txp;
    if (cur_txp == 0xFF) cur_txp = PA_MAX_OUTPUT;  // Default to board max

    // Default frequency if not set
    if (cur_freq == 0) cur_freq = 914875000;  // 914.875 MHz default
    if (cur_bw == 0)   cur_bw   = 125000;     // 125 kHz default
    if (cur_sf == 0)   cur_sf   = 10;         // SF10 default
    if (cur_cr < 5 || cur_cr > 8) cur_cr = 5; // CR 4/5 default

    // Build the HTML page
    String html = F(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RNode Boundary Setup</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;margin:0;padding:16px;}"
        "h1{color:#e94560;font-size:1.4em;margin:0 0 8px;}"
        "h2{color:#0f3460;background:#e0e0e0;padding:6px 10px;margin:18px -10px 10px;font-size:1em;border-radius:4px;}"
        "form{max-width:480px;margin:0 auto;}"
        "label{display:block;margin:8px 0 2px;font-size:0.9em;color:#aaa;}"
        "input,select{width:100%;padding:8px;margin:2px 0 6px;box-sizing:border-box;"
        "background:#16213e;border:1px solid #0f3460;color:#e0e0e0;border-radius:4px;font-size:0.95em;}"
        "input:focus,select:focus{border-color:#e94560;outline:none;}"
        ".row{display:flex;gap:10px;}.row>div{flex:1;}"
        ".note{font-size:0.8em;color:#666;margin:2px 0 8px;}"
        "button{width:100%;padding:12px;margin:20px 0;background:#e94560;color:#fff;"
        "border:none;border-radius:4px;font-size:1.1em;cursor:pointer;}"
        "button:hover{background:#c73e54;}"
        ".ok{background:#16213e;padding:20px;border-radius:8px;text-align:center;}"
        ".ok h1{color:#0f0;}"
        ".node-hash{background:#0f1a30;border:1px solid #0f3460;border-radius:6px;"
        "padding:10px 14px;margin:0 0 16px;}"
        ".node-hash .nh-label{display:block;font-size:0.75em;color:#888;margin-bottom:4px;}"
        ".node-hash code{font-family:monospace;font-size:0.95em;color:#7ecfff;"
        "word-break:break-all;letter-spacing:0.05em;}"
        "</style></head><body>"
        "<h1>&#x1f4e1; RNode Boundary Node</h1>"
    );

    // ── Node public hash ──
    html += F("<div class='node-hash'><span class='nh-label'>&#x1f511; Node Hash (Reticulum destination)</span><code>");
    if (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0') {
        html += String(rtc_node_hash_hex);
    } else {
        html += F("<span style='color:#888;font-style:italic;'>Not yet assigned &mdash; will be set on first normal boot</span>");
    }
    html += F("</code></div>");

    html += F("<form method='POST' action='/save'>");
    html += F(
        "<h2>&#x1f4f6; WiFi Network</h2>"
        "<label>WiFi</label>"
        "<select name='wifi_en'>"
    );
    html += F("<option value='1'");
    if (boundary_state.wifi_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("<option value='0'");
    if (!boundary_state.wifi_enabled) html += F(" selected");
    html += F(">Disabled (LoRa-only repeater)</option>");
    html += F("</select>");

    html += F(
        "<label>SSID</label>"
        "<input name='ssid' maxlength='32' placeholder='Your WiFi network' value='"
    );
    html += String(cur_ssid);
    html += F(
        "'>"
        "<label>Password</label>"
        "<input name='psk' type='password' maxlength='32' placeholder='WiFi password' value='"
    );
    html += String(cur_psk);
    html += F("'>");

    // ── TCP Backbone Section ──
    html += F(
        "<h2>&#x1f310; TCP Backbone</h2>"
        "<label>Mode</label>"
        "<select name='tcp_mode'>"
    );
    html += F("<option value='0'");
    if (boundary_state.tcp_mode == 0) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (boundary_state.tcp_mode == 1) html += F(" selected");
    html += F(">Client (connect to backbone)</option>");
    html += F("</select>");

    html += F("<label>Backbone Host</label>");
    html += F("<input name='bb_host' maxlength='63' placeholder='e.g. 192.168.1.100' value='");
    html += String(boundary_state.backbone_host);
    html += F("'>");

    html += F("<label>Backbone Port</label>");
    html += F("<input name='bb_port' type='number' min='1' max='65535' value='");
    html += String(boundary_state.backbone_port);
    html += F("'>");

    // ── Local TCP Server Section ──
    html += F(
        "<h2>&#x1f4e1; Local TCP Server (optional)</h2>"
        "<p class='note'>Run a TCP server on the same WiFi network so local devices can connect. "
        "Uses Access Point mode (does not forward announces).</p>"
        "<label>Local TCP Server</label>"
        "<select name='ap_tcp_en'>"
    );
    html += F("<option value='0'");
    if (!boundary_state.ap_tcp_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (boundary_state.ap_tcp_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    html += F("<label>TCP Port</label>");
    html += F("<input name='ap_tcp_port' type='number' min='1' max='65535' value='");
    html += String(boundary_state.ap_tcp_port);
    html += F("'>");

    // ── LoRa Radio Section ──
    html += F(
        "<h2>&#x1f4fb; LoRa Radio</h2>"
    );

    // Frequency — show in MHz for human-friendliness
    char freq_str[16];
    dtostrf((double)cur_freq / 1000000.0, 1, 3, freq_str);
    html += F("<label>Frequency (MHz)</label>");
    html += F("<input name='freq' type='text' placeholder='914.875' value='");
    html += String(freq_str);
    html += F("'>");
    html += F("<p class='note'>e.g. 914.875, 868.000, 433.000</p>");

    // Bandwidth — dropdown
    html += F("<label>Bandwidth</label><select name='bw'>");
    for (int i = 0; i < BW_OPTIONS_COUNT; i++) {
        uint32_t bw_hz = pgm_read_dword(&BW_OPTIONS_HZ[i]);
        char label_buf[16];
        strncpy_P(label_buf, (const char*)pgm_read_ptr(&BW_OPTIONS_LABELS[i]), sizeof(label_buf)-1);
        label_buf[sizeof(label_buf)-1] = '\0';
        html += F("<option value='");
        html += String(bw_hz);
        html += "'";
        if (bw_hz == cur_bw) html += F(" selected");
        html += ">";
        html += label_buf;
        html += F("</option>");
    }
    html += F("</select>");

    // Spreading Factor — dropdown 5-12
    html += F("<label>Spreading Factor</label><select name='sf'>");
    for (int sf = 5; sf <= 12; sf++) {
        html += F("<option value='");
        html += String(sf);
        html += "'";
        if (sf == cur_sf) html += F(" selected");
        html += ">SF";
        html += String(sf);
        html += F("</option>");
    }
    html += F("</select>");

    // Coding Rate — dropdown 5-8 (maps to 4/5 through 4/8)
    html += F("<label>Coding Rate</label><select name='cr'>");
    for (int cr = 5; cr <= 8; cr++) {
        html += F("<option value='");
        html += String(cr);
        html += "'";
        if (cr == cur_cr) html += F(" selected");
        html += ">4/";
        html += String(cr);
        html += F("</option>");
    }
    html += F("</select>");

    // TX Power
    html += F("<label>TX Power (dBm)</label>");
    html += F("<input name='txp' type='number' min='2' max='");
    #ifdef PA_MAX_OUTPUT
    html += String(PA_MAX_OUTPUT);
    #else
    html += "22";
    #endif
    html += F("' value='");
    html += String(cur_txp);
    html += F("'>");

    #ifdef PA_MAX_OUTPUT
    html += F("<p class='note'>Max output for this board: ");
    html += String(PA_MAX_OUTPUT);
    html += F(" dBm (with PA)</p>");
    #endif

    // ── IFAC (Interface Access Code) Section ──
    html += F(
        "<h2>&#x1f512; Network Access (IFAC)</h2>"
        "<p class='note'>Set a network name and/or passphrase to restrict LoRa interface access. "
        "Only nodes with matching settings can communicate. Both fields are optional.</p>"
        "<label>IFAC</label>"
        "<select name='ifac_en'>"
    );
    html += F("<option value='0'");
    if (!boundary_state.ifac_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (boundary_state.ifac_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    html += F("<label>Network Name</label>");
    html += F("<input name='ifac_name' maxlength='32' placeholder='e.g. MyNetwork' value='");
    html += String(boundary_state.ifac_netname);
    html += F("'>");

    html += F("<label>Passphrase</label>");
    html += F("<input name='ifac_pass' type='password' maxlength='32' placeholder='Shared secret' value='");
    html += String(boundary_state.ifac_passphrase);
    html += F("'>");

    // ── Device Advertisement Section ──
    html += F(
        "<h2>&#x1f4cd; Device Advertisement</h2>"
        "<p class='note'>Advertise this node and its parameters on the Reticulum network. "
        "Maps such as <a href='https://rmap.world' target='_blank' style='color:#7ecfff'>rmap.world</a> "
        "use these announcements to automatically place a pin for the node. "
        "Disabled by default; enable only if you want this node to be publicly listed.</p>"
        "<label>Advertise Device</label>"
        "<select name='advert_en'>"
    );
    html += F("<option value='0'");
    if (!boundary_state.advert_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (boundary_state.advert_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    // Latitude / Longitude — pre-populate with current values, but keep
    // the inputs blank when the user has not yet set coordinates so the
    // browser placeholder hint is visible.
    char lat_str[32];
    char lon_str[32];
    lat_str[0] = '\0';
    lon_str[0] = '\0';
    if (boundary_state.advert_enabled ||
        boundary_state.advert_lat != 0.0 ||
        boundary_state.advert_lon != 0.0) {
        dtostrf(boundary_state.advert_lat, 1, 6, lat_str);
        dtostrf(boundary_state.advert_lon, 1, 6, lon_str);
    }

    html += F("<div class='row'>");
    html += F("<div><label>Latitude (&deg;)</label>");
    html += F("<input id='advert_lat' name='advert_lat' type='text' inputmode='decimal' "
              "placeholder='e.g. 37.774929' value='");
    html += String(lat_str);
    html += F("'></div>");
    html += F("<div><label>Longitude (&deg;)</label>");
    html += F("<input id='advert_lon' name='advert_lon' type='text' inputmode='decimal' "
              "placeholder='e.g. -122.419416' value='");
    html += String(lon_str);
    html += F("'></div>");
    html += F("</div>");
    html += F("<p class='note'>Decimal degrees, signed. North/East positive, South/West negative. "
              "Leave both blank to omit coordinates.</p>");

    // Browser geolocation helper button. Geolocation may be blocked on
    // plain HTTP origins by some browsers; the script reports an error
    // inline if the request fails or is denied.
    html += F(
        "<button type='button' id='geo_btn' "
        "style='width:100%;padding:10px;margin:4px 0 6px;background:#0f3460;"
        "color:#fff;border:none;border-radius:4px;font-size:0.95em;cursor:pointer;'>"
        "&#x1f4cd; Use Browser Location</button>"
        "<p id='geo_status' class='note' style='min-height:1em;'></p>"
        "<script>"
        "(function(){"
        "var btn=document.getElementById('geo_btn');"
        "var st=document.getElementById('geo_status');"
        "if(!btn)return;"
        "btn.addEventListener('click',function(){"
        "if(!('geolocation' in navigator)){"
        "st.textContent='Geolocation not supported by this browser.';return;}"
        "st.textContent='Requesting location\\u2026';"
        "navigator.geolocation.getCurrentPosition(function(pos){"
        "document.getElementById('advert_lat').value=pos.coords.latitude.toFixed(6);"
        "document.getElementById('advert_lon').value=pos.coords.longitude.toFixed(6);"
        "st.textContent='Location filled (\\u00b1'+Math.round(pos.coords.accuracy)+' m).';"
        "},function(err){"
        "st.textContent='Could not get location: '+err.message;"
        "},{enableHighAccuracy:true,timeout:15000,maximumAge:0});"
        "});"
        "})();"
        "</script>"
    );

    html += F("<label>Randomize Offset</label>"
              "<select name='advert_jitter'>");
    html += F("<option value='0'");
    if (!boundary_state.advert_jitter) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (boundary_state.advert_jitter) html += F(" selected");
    html += F(">Enabled (~0.5 km / 0.5 mi)</option>");
    html += F("</select>");
    html += F("<p class='note'>When enabled, the advertised coordinates are shifted by a "
              "random offset of approximately half a kilometre (about half a mile) for "
              "privacy. The exact stored coordinates are not changed.</p>");

    // ── Options Section ──
    html += F(
        "<h2>&#x2699; Options</h2>"
        "<label>Display Blanking</label>"
        "<select name='disp_blank'>"
    );

    // Read current blanking timeout from EEPROM (stored as minutes, 0 = never)
    uint8_t cur_blank = 5;
    if (EEPROM.read(eeprom_addr(ADDR_CONF_BSET)) == CONF_OK_BYTE) {
        cur_blank = EEPROM.read(eeprom_addr(ADDR_CONF_DBLK));
    }

    uint8_t cur_rotation = EEPROM.read(eeprom_addr(ADDR_CONF_DROT));
    if (cur_rotation > 3) {
        cur_rotation = config_default_display_rotation();
    }

    static const uint8_t blank_vals[]   = { 0, 1, 5, 10, 30, 60 };
    static const char* blank_labels[]   = { "Never", "1 minute", "5 minutes", "10 minutes", "30 minutes", "60 minutes" };
    static const int blank_count = 6;

    for (int i = 0; i < blank_count; i++) {
        html += F("<option value='");
        html += String(blank_vals[i]);
        html += "'";
        if (blank_vals[i] == cur_blank) html += F(" selected");
        html += ">";
        html += blank_labels[i];
        html += F("</option>");
    }
    html += F("</select>");
    html += F("<p class='note'>Turn off display after inactivity to save power</p>");

    html += F("<label>Display Orientation</label><select name='disp_rot'>");
    html += F("<option value='0'");
    if (cur_rotation == 0) html += F(" selected");
    html += F(">Landscape</option>");
    html += F("<option value='1'");
    if (cur_rotation == 1) html += F(" selected");
    html += F(">Portrait</option>");
    html += F("<option value='2'");
    if (cur_rotation == 2) html += F(" selected");
    html += F(">Landscape Flipped</option>");
    html += F("<option value='3'");
    if (cur_rotation == 3) html += F(" selected");
    html += F(">Portrait Flipped</option>");
    html += F("</select>");
    html += F("<p class='note'>Choose the orientation that matches your OLED mounting. "
              "Landscape modes place the two status panes side by side; portrait modes stack them.</p>");

    // ── Submit ──
    html += F(
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></body></html>"
    );

    config_server->send(200, "text/html", html);
}

// ─── Handle POST /save ──────────────────────────────────────────────────────

static void config_handle_save() {
    // ── WiFi STA credentials ──
    String ssid = config_server->arg("ssid");
    String psk  = config_server->arg("psk");

    // Write SSID to config EEPROM area
    for (int i = 0; i < 32; i++) {
        uint8_t c = (i < (int)ssid.length()) ? ssid[i] : 0x00;
        EEPROM.write(config_addr(ADDR_CONF_SSID + i), c);
    }
    EEPROM.write(config_addr(ADDR_CONF_SSID + 32), 0x00);

    // Write PSK
    for (int i = 0; i < 32; i++) {
        uint8_t c = (i < (int)psk.length()) ? psk[i] : 0x00;
        EEPROM.write(config_addr(ADDR_CONF_PSK + i), c);
    }
    EEPROM.write(config_addr(ADDR_CONF_PSK + 32), 0x00);

    // Set WiFi mode to STA
    EEPROM.write(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_STA);

    // Boundary mode always uses DHCP on the STA interface. Clear the legacy
    // static IP and netmask slots so stale values from older firmware or tools
    // cannot force a persistent static address.
    for (int i = 0; i < 4; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IP + i), 0x00);
        EEPROM.write(config_addr(ADDR_CONF_NM + i), 0x00);
    }

    // ── WiFi enable setting ──
    boundary_state.wifi_enabled = (config_server->arg("wifi_en").toInt() == 1);

    // ── Display blanking (EEPROM stores minutes, 0 = disabled) ──
    int blank_minutes = config_server->arg("disp_blank").toInt();
    if (blank_minutes <= 0) {
        display_blanking_enabled = false;
        eeprom_update(eeprom_addr(ADDR_CONF_BSET), CONF_OK_BYTE);
        eeprom_update(eeprom_addr(ADDR_CONF_DBLK), 0);
    } else {
        uint8_t blank_val = (uint8_t)(blank_minutes > 255 ? 255 : blank_minutes);
        display_blanking_enabled = true;
        display_blanking_timeout = (uint32_t)blank_val * 60UL * 1000UL;
        eeprom_update(eeprom_addr(ADDR_CONF_BSET), CONF_OK_BYTE);
        eeprom_update(eeprom_addr(ADDR_CONF_DBLK), blank_val);
    }

    int display_rotation = config_server->arg("disp_rot").toInt();
    if (display_rotation < 0 || display_rotation > 3) {
        display_rotation = config_default_display_rotation();
    }
    eeprom_update(eeprom_addr(ADDR_CONF_DROT), (uint8_t)display_rotation);

    // ── TCP backbone settings ──
    boundary_state.tcp_mode = (uint8_t)config_server->arg("tcp_mode").toInt(); // 0=disabled, 1=client
    if (boundary_state.tcp_mode > 1) boundary_state.tcp_mode = 0;
    boundary_state.tcp_port = (uint16_t)config_server->arg("tcp_port").toInt();
    if (boundary_state.tcp_port == 0) boundary_state.tcp_port = 4242;

    String bb_host = config_server->arg("bb_host");
    memset(boundary_state.backbone_host, 0, sizeof(boundary_state.backbone_host));
    strncpy(boundary_state.backbone_host, bb_host.c_str(), sizeof(boundary_state.backbone_host) - 1);

    boundary_state.backbone_port = (uint16_t)config_server->arg("bb_port").toInt();
    if (boundary_state.backbone_port == 0) boundary_state.backbone_port = 4242;

    // ── Local TCP server settings ──
    boundary_state.ap_tcp_enabled = (config_server->arg("ap_tcp_en").toInt() == 1);
    boundary_state.ap_tcp_port = (uint16_t)config_server->arg("ap_tcp_port").toInt();
    if (boundary_state.ap_tcp_port == 0) boundary_state.ap_tcp_port = 4242;

    // ── IFAC settings ──
    boundary_state.ifac_enabled = (config_server->arg("ifac_en").toInt() == 1);

    String ifac_name = config_server->arg("ifac_name");
    memset(boundary_state.ifac_netname, 0, sizeof(boundary_state.ifac_netname));
    strncpy(boundary_state.ifac_netname, ifac_name.c_str(), sizeof(boundary_state.ifac_netname) - 1);

    String ifac_pass = config_server->arg("ifac_pass");
    memset(boundary_state.ifac_passphrase, 0, sizeof(boundary_state.ifac_passphrase));
    strncpy(boundary_state.ifac_passphrase, ifac_pass.c_str(), sizeof(boundary_state.ifac_passphrase) - 1);

    // If IFAC is enabled but both fields are empty, disable it
    if (boundary_state.ifac_enabled &&
        boundary_state.ifac_netname[0] == '\0' &&
        boundary_state.ifac_passphrase[0] == '\0') {
        boundary_state.ifac_enabled = false;
    }

    // ── Device advertisement settings ──
    boundary_state.advert_enabled = (config_server->arg("advert_en").toInt() == 1);

    // Empty lat/lon strings are treated as "not set" → 0.0. Otherwise parse
    // and clamp to valid ranges; out-of-range values are silently coerced
    // to 0.0 rather than rejecting the whole save.
    String lat_arg = config_server->arg("advert_lat");
    String lon_arg = config_server->arg("advert_lon");
    lat_arg.trim();
    lon_arg.trim();
    if (lat_arg.length() == 0) {
        boundary_state.advert_lat = 0.0;
    } else {
        double lat_val = lat_arg.toDouble();
        if (lat_val < -90.0 || lat_val > 90.0 || isnan(lat_val)) {
            lat_val = 0.0;
        }
        boundary_state.advert_lat = lat_val;
    }
    if (lon_arg.length() == 0) {
        boundary_state.advert_lon = 0.0;
    } else {
        double lon_val = lon_arg.toDouble();
        if (lon_val < -180.0 || lon_val > 180.0 || isnan(lon_val)) {
            lon_val = 0.0;
        }
        boundary_state.advert_lon = lon_val;
    }

    boundary_state.advert_jitter = (config_server->arg("advert_jitter").toInt() == 1);

    // Save boundary config to EEPROM
    boundary_save_config();

    // ── LoRa radio settings ──
    String freq_str = config_server->arg("freq");
    double freq_mhz = freq_str.toDouble();
    if (freq_mhz > 0) {
        lora_freq = (uint32_t)(freq_mhz * 1000000.0);
    }

    String bw_str = config_server->arg("bw");
    uint32_t bw_val = (uint32_t)bw_str.toInt();
    if (bw_val > 0) lora_bw = bw_val;

    int sf_val = config_server->arg("sf").toInt();
    if (sf_val >= 5 && sf_val <= 12) lora_sf = sf_val;

    int cr_val = config_server->arg("cr").toInt();
    if (cr_val >= 5 && cr_val <= 8) lora_cr = cr_val;

    int txp_val = config_server->arg("txp").toInt();
    if (txp_val >= 2 && txp_val <= 30) lora_txp = txp_val;

    // Save LoRa config to EEPROM (reuse existing eeprom_conf functions)
    // Write directly since hw_ready may not be set yet
    eeprom_update(eeprom_addr(ADDR_CONF_SF), lora_sf);
    eeprom_update(eeprom_addr(ADDR_CONF_CR), lora_cr);
    eeprom_update(eeprom_addr(ADDR_CONF_TXP), lora_txp);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 0, lora_bw >> 24);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 1, lora_bw >> 16);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 2, lora_bw >> 8);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 3, lora_bw);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 0, lora_freq >> 24);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 1, lora_freq >> 16);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 2, lora_freq >> 8);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 3, lora_freq);
    eeprom_update(eeprom_addr(ADDR_CONF_OK), CONF_OK_BYTE);

    EEPROM.commit();

    // ── Send confirmation page ──
    String ok = F(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:40px;"
        "display:flex;align-items:center;justify-content:center;min-height:80vh;}"
        ".ok{background:#16213e;padding:30px;border-radius:12px;text-align:center;max-width:400px;}"
        "h1{color:#4caf50;margin-bottom:16px;}"
        "p{color:#aaa;}"
        "</style></head><body>"
        "<div class='ok'>"
        "<h1>&#x2705; Configuration Saved</h1>"
        "<p>Device will reboot in 3 seconds and connect to your WiFi network.</p>"
        "<p style='color:#666;font-size:0.85em;'>If the device cannot connect, hold the button for 5+ seconds to re-enter setup.</p>"
        "</div></body></html>"
    );
    config_server->send(200, "text/html", ok);

    // Give the response time to send
    delay(3000);

    // Reboot
    ESP.restart();
}

// ─── Captive Portal redirect ─────────────────────────────────────────────────
static void config_handle_redirect() {
    config_server->sendHeader("Location", "http://10.0.0.1/", true);
    config_server->send(302, "text/plain", "Redirecting to setup...");
}

// ─── Check if config is needed ───────────────────────────────────────────────
bool boundary_needs_config() {
    // If the RTNode app marker is missing, this node was either never
    // configured by RTNode or was flashed from a different firmware family
    // such as stock RNode. Force the portal so RTNode can claim and rewrite
    // its persisted settings explicitly.
    if (!boundary_app_marker_valid()) {
        return true;
    }

    // Check if WiFi SSID is configured
    char ssid[33];
    for (int i = 0; i < 32; i++) {
        ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        if (ssid[i] == (char)0xFF) ssid[i] = '\0';
    }
    ssid[32] = '\0';

    // Also check boundary mode enable flag
    uint8_t bmode = EEPROM.read(config_addr(ADDR_CONF_BMODE));

    // Need config if no SSID set and boundary not yet configured
    if (ssid[0] == '\0' && bmode != BOUNDARY_ENABLE_BYTE) {
        return true;
    }
    return false;
}

// ─── Start Config Portal ─────────────────────────────────────────────────────
void config_portal_start() {
    if (config_portal_active) return;

    Serial.println("[Config] Starting configuration portal...");

    // Stop any existing WiFi
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);

    // Start AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_AP_SSID, NULL);  // Open AP for easy setup
    delay(150);

    IPAddress ap_addr(10, 0, 0, 1);
    IPAddress ap_mask(255, 255, 255, 0);
    WiFi.softAPConfig(ap_addr, ap_addr, ap_mask);

    Serial.print("[Config] AP started: ");
    Serial.println(CONFIG_AP_SSID);
    Serial.print("[Config] IP: ");
    Serial.println(WiFi.softAPIP());

    // Start DNS server for captive portal (redirect all domains to us)
    config_dns = new DNSServer();
    config_dns->start(DNS_PORT, "*", ap_addr);

    // Start web server
    config_server = new WebServer(HTTP_PORT);
    config_server->on("/", HTTP_GET, config_send_html);
    config_server->on("/save", HTTP_POST, config_handle_save);
    config_server->onNotFound(config_handle_redirect);  // Captive portal catch-all
    config_server->begin();

    config_portal_active = true;

    Serial.println("[Config] Portal ready — connect to WiFi: " + String(CONFIG_AP_SSID));

    #if HAS_DISPLAY
    if (disp_ready) {
        // Show config mode on display
        stat_area.fillScreen(SSD1306_BLACK);
        stat_area.setCursor(0, 0);
        stat_area.println("CONFIG MODE");
        stat_area.println("");
        stat_area.println("Connect to:");
        stat_area.println(CONFIG_AP_SSID);
        stat_area.println("");
        stat_area.println("Open browser");
        stat_area.println("http://10.0.0.1");
        display.clearDisplay();
        display.drawBitmap(0, 0, stat_area.getBuffer(), stat_area.width(), stat_area.height(), SSD1306_WHITE, SSD1306_BLACK);
        display.display();
    }
    #endif
    // Headless: LED ramp will be driven from the WCC portal loop
    if (headless_mode) {
        Serial.println("[Config] Headless mode — LED will breathe during config portal");
    }
}

// ─── Stop Config Portal ──────────────────────────────────────────────────────
void config_portal_stop() {
    if (!config_portal_active) return;

    Serial.println("[Config] Stopping configuration portal");

    if (config_server) {
        config_server->stop();
        delete config_server;
        config_server = nullptr;
    }
    if (config_dns) {
        config_dns->stop();
        delete config_dns;
        config_dns = nullptr;
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
    config_portal_active = false;
}

// ─── Portal Loop — call from main loop() ─────────────────────────────────────
void config_portal_loop() {
    if (!config_portal_active) return;
    if (config_dns)    config_dns->processNextRequest();
    if (config_server) config_server->handleClient();
}

// ─── Is portal active? ──────────────────────────────────────────────────────
bool config_portal_is_active() {
    return config_portal_active;
}

#endif // BOUNDARY_MODE
#endif // BOUNDARY_CONFIG_H
