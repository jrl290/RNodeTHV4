// Copyright (C) 2026, Boundary Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// BoundaryConfig.h — Captive-portal web configuration for Boundary Mode.
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
struct BwOption { uint32_t hz; const char* label; };
static const BwOption BW_OPTIONS[] = {
    {  7800, "7.8 kHz"   },
    { 10400, "10.4 kHz"  },
    { 15600, "15.6 kHz"  },
    { 20800, "20.8 kHz"  },
    { 31250, "31.25 kHz" },
    { 41700, "41.7 kHz"  },
    { 62500, "62.5 kHz"  },
    {125000, "125 kHz"   },
    {250000, "250 kHz"   },
    {500000, "500 kHz"   },
};
static const int BW_OPTIONS_COUNT = sizeof(BW_OPTIONS) / sizeof(BW_OPTIONS[0]);

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
    if (cur_txp == 0xFF) cur_txp = PA_MAX_OUTPUT;  // Default max TX power

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
        "</style></head><body>"
        "<h1>&#x1f4e1; RNode Boundary Node</h1>"
        "<form method='POST' action='/save'>"
    );

    // ── WiFi STA Section ──
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
        html += F("<option value='");
        html += String(BW_OPTIONS[i].hz);
        html += "'";
        if (BW_OPTIONS[i].hz == cur_bw) html += F(" selected");
        html += ">";
        html += BW_OPTIONS[i].label;
        html += F("</option>");
    }
    html += F("</select>");

    // Spreading Factor — dropdown 6-12
    html += F("<label>Spreading Factor</label><select name='sf'>");
    for (int sf = 6; sf <= 12; sf++) {
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
    html += String(PA_MAX_OUTPUT);
    html += F("' value='");
    html += String(cur_txp);
    html += F("'>");

    #ifdef PA_MAX_OUTPUT
    html += F("<p class='note'>Max output for this board: ");
    html += String(PA_MAX_OUTPUT);
    html += F(" dBm (with PA)</p>");
    #endif

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

    // ── WiFi enable setting ──
    boundary_state.wifi_enabled = (config_server->arg("wifi_en").toInt() == 1);

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
    if (sf_val >= 6 && sf_val <= 12) lora_sf = sf_val;

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
