// mDNS / zeroconf service for RTNode
//
// Publishes the node's STA hostname and a `_reticulum._tcp` service so peers
// on the same LAN can discover the local TCP server without a static IP or
// router DHCP reservation. In config-portal AP mode publishes a fixed name
// (`rnode-setup.local`) so the captive portal can be reached by name.
//
// Header-only; safe to include from multiple translation units thanks to the
// `inline` linkage and a single static state variable per build.

#pragma once

#include <ESPmDNS.h>
#include <WiFi.h>

namespace mdns_service {

inline bool& running_ref() {
    static bool running = false;
    return running;
}

// Normalize a hostname for mDNS: lowercase ASCII, replace anything outside
// [a-z0-9-] with '-'. Writes at most `out_size-1` chars + NUL into `out`.
inline void normalize_hostname(const char* in, char* out, size_t out_size) {
    if (out_size == 0) return;
    size_t j = 0;
    for (size_t i = 0; in[i] != 0 && j + 1 < out_size; ++i) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        out[j++] = ok ? c : '-';
    }
    out[j] = 0;
}

// Resolve the effective mDNS hostname: honor `custom` if non-empty, otherwise
// fall back to `rtnode<XXXX>` where XXXX = `default_suffix` (typically the
// last 4 hex chars of the device MAC). Output is normalized.
inline void resolve_hostname(const char* custom, const char* default_suffix,
                             char* out, size_t out_size) {
    if (custom != nullptr && custom[0] != 0) {
        normalize_hostname(custom, out, out_size);
    } else {
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "rtnode%s",
                 default_suffix != nullptr ? default_suffix : "");
        normalize_hostname(fallback, out, out_size);
    }
}

// Start mDNS in STA mode and (optionally) advertise the local TCP server.
// `hostname` is the .local name — case and stray chars are normalized.
// `tcp_port == 0` means "do not advertise the _reticulum._tcp service".
inline bool start_sta(const char* hostname, uint16_t tcp_port) {
    bool& running = running_ref();
    if (running) return true;
    if (hostname == nullptr || hostname[0] == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    char norm[32];
    normalize_hostname(hostname, norm, sizeof(norm));
    if (norm[0] == 0) return false;

    if (!MDNS.begin(norm)) {
        Serial.println("[mDNS] begin() failed in STA mode");
        return false;
    }
    if (tcp_port != 0) {
        MDNS.addService("reticulum", "tcp", tcp_port);
        MDNS.addServiceTxt("reticulum", "tcp", "kind", "rtnode-heltec");
    }
    running = true;
    Serial.print("[mDNS] STA up: ");
    Serial.print(norm);
    Serial.print(".local");
    if (tcp_port != 0) {
        Serial.print(" (_reticulum._tcp port ");
        Serial.print(tcp_port);
        Serial.print(")");
    }
    Serial.println();
    return true;
}

// Start mDNS in AP mode for the config portal. Hostname is fixed so users can
// always type the same .local address regardless of which device they are
// configuring.
inline bool start_ap_config(const char* hostname) {
    bool& running = running_ref();
    if (running) return true;
    if (hostname == nullptr || hostname[0] == 0) return false;

    if (!MDNS.begin(hostname)) {
        Serial.println("[mDNS] begin() failed in AP mode");
        return false;
    }
    MDNS.addService("http", "tcp", 80);
    running = true;
    Serial.print("[mDNS] AP up: ");
    Serial.print(hostname);
    Serial.println(".local");
    return true;
}

// Convenience: derive the MAC-suffix fallback automatically and start the STA
// service. Equivalent to manually calling resolve_hostname() + start_sta().
inline bool start_sta_auto(const char* custom, uint16_t tcp_port) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char suffix[5];
    snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);
    char name[33];
    resolve_hostname(custom, suffix, name, sizeof(name));
    return start_sta(name, tcp_port);
}

inline void stop() {
    bool& running = running_ref();
    if (!running) return;
    MDNS.end();
    running = false;
    Serial.println("[mDNS] stopped");
}

inline bool is_running() { return running_ref(); }

}  // namespace mdns_service
