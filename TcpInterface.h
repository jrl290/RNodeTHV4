// Copyright (C) 2026, Boundary Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// TcpInterface — An RNS InterfaceImpl that bridges a WiFi TCP
// connection as an RNS transport interface. Used for both the
// backbone (BackboneInterface) and local AP (LocalTcpInterface).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef TCP_INTERFACE_H
#define TCP_INTERFACE_H

#ifdef HAS_RNS
#ifdef BOUNDARY_MODE

#include <WiFi.h>
#include <lwip/sockets.h>   // SO_LINGER — force RST to free lwIP PCBs immediately
#include <Interface.h>
#include <Transport.h>
#include <Bytes.h>

// ─── TCP Interface Configuration ─────────────────────────────────────────────
#define TCP_IF_DEFAULT_PORT      4242
#ifdef BOUNDARY_MODE
#define TCP_IF_MAX_CLIENTS       8
#else
#define TCP_IF_MAX_CLIENTS       4
#endif
#define TCP_IF_HW_MTU            1064
#define TCP_IF_CONNECT_TIMEOUT   6000    // ms
#define TCP_IF_WRITE_TIMEOUT     2000    // ms — short to avoid WDT
#define TCP_IF_READ_TIMEOUT      120000  // ms — 2 minutes (backbone can go quiet)
#define TCP_IF_RECONNECT_MIN     10000   // ms — initial reconnect interval
#define TCP_IF_RECONNECT_MAX     120000  // ms — max backoff (2 minutes)
#define TCP_IF_KEEPALIVE_INTERVAL 30000  // ms — send empty HDLC frames to keep link alive
#define TCP_IF_POLL_INTERVAL     10      // ms

// HDLC-like framing for TCP (matches Reticulum-rust tcp_interface)
#define HDLC_FLAG  0x7E
#define HDLC_ESC   0x7D
#define HDLC_ESC_MASK 0x20

// ─── TCP Interface Mode ──────────────────────────────────────────────────────
enum TcpIfMode {
    TCP_IF_MODE_SERVER = 0,  // Listen for incoming connections (from backbone rnsd)
    TCP_IF_MODE_CLIENT = 1,  // Connect out to a backbone rnsd TCP server
};

// ─── Client connection state ─────────────────────────────────────────────────
struct TcpClient {
    WiFiClient client;
    uint32_t   last_activity;
    bool       active;
    // HDLC deframe state
    bool       in_frame;
    bool       escape;
    bool       truncated;
    uint8_t    rxbuf[TCP_IF_HW_MTU];
    uint16_t   rxlen;
};

// ─── TcpInterface Class ─────────────────────────────────────────────────────
class TcpInterface : public RNS::InterfaceImpl {
public:
    TcpInterface(TcpIfMode mode, uint16_t port = TCP_IF_DEFAULT_PORT,
                 const char* target_host = nullptr, uint16_t target_port = 0,
                 const char* name = "BackboneInterface")
        : RNS::InterfaceImpl(name),
          _mode(mode),
          _port(port),
          _target_port(target_port),
          _server(nullptr),
          _num_clients(0),
          _last_reconnect(0),
          _last_keepalive(0),
          _reconnect_interval(TCP_IF_RECONNECT_MIN),
          _read_timeout(TCP_IF_READ_TIMEOUT),
          _resolved_ip((uint32_t)0),
          _consecutive_failures(0),
          _started(false)
    {
        _IN = true;
        _OUT = true;
        _HW_MTU = TCP_IF_HW_MTU;
        // v1.0.12: Tell Transport this interface has a known fixed MTU,
        // enabling link MTU clamping when forwarding LINKREQUEST packets.
        _FIXED_MTU = true;
        // TCP links are effectively 10 Mbps+. Setting a realistic
        // bitrate lets Transport prefer TCP paths over LoRa when
        // both exist for the same destination.
        // announce_cap = 2% keeps backbone announce flooding in check.
        _bitrate = 10000000;
        _announce_cap = 2.0;
        if (target_host != nullptr) {
            strncpy(_target_host, target_host, sizeof(_target_host) - 1);
            _target_host[sizeof(_target_host) - 1] = '\0';
        } else {
            _target_host[0] = '\0';
        }
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            _clients[i].active = false;
            _clients[i].in_frame = false;
            _clients[i].escape = false;
            _clients[i].truncated = false;
            _clients[i].rxlen = 0;
            _clients[i].last_activity = 0;
        }
    }

    virtual ~TcpInterface() {
        stop();
    }

    // ─── Lifecycle ───────────────────────────────────────────────────────────
    bool start() {
        if (_started) return true;

        if (_mode == TCP_IF_MODE_SERVER) {
            _server = new WiFiServer(_port, TCP_IF_MAX_CLIENTS);
            _server->begin();
            _server->setNoDelay(true);
            Serial.printf("[TcpIF] Server listening on port %d\r\n", _port);
            _started = true;
        } else {
            // Client mode — try initial connection
            _started = true;
            _connect_client();
        }
        return _started;
    }

    void stop() {
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (_clients[i].active) {
                // Force RST to free lwIP PCBs immediately (no TIME_WAIT)
                int fd = _clients[i].client.fd();
                if (fd >= 0) {
                    struct linger lin;
                    lin.l_onoff = 1;
                    lin.l_linger = 0;
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
                }
                _clients[i].client.stop();
                _clients[i].client = WiFiClient();
                _clients[i].active = false;
            }
        }
        if (_server) {
            _server->end();
            delete _server;
            _server = nullptr;
        }
        _started = false;
        _num_clients = 0;
    }

    // ─── Main loop — call from Arduino loop() ────────────────────────────────
    void loop() {
        if (!_started) return;

        // Accept new connections in server mode
        if (_mode == TCP_IF_MODE_SERVER && _server) {
            WiFiClient newClient = _server->available();
            if (newClient) {
                _accept_client(newClient);
            }
        }

        // Client mode reconnection (with WiFi check + exponential backoff)
        if (_mode == TCP_IF_MODE_CLIENT && _num_clients == 0) {
            uint32_t now = millis();
            if (now - _last_reconnect >= _reconnect_interval) {
                if (WiFi.status() == WL_CONNECTED) {
                    _connect_client();
                } else {
                    // WiFi not connected — skip TCP attempt, just update timer
                    _last_reconnect = now;
                }
            }
        }

        // Send keepalive (empty HDLC frames) to prevent read timeout on both sides
        if (_num_clients > 0) {
            uint32_t now = millis();
            if (now - _last_keepalive >= TCP_IF_KEEPALIVE_INTERVAL) {
                _last_keepalive = now;
                uint8_t ka[] = { HDLC_FLAG, HDLC_FLAG };
                for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
                    if (_clients[i].active && _clients[i].client.connected()) {
                        _clients[i].client.write(ka, 2);
                    }
                }
            }

        }

        // Process incoming data from all active clients
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (!_clients[i].active) continue;

            if (!_clients[i].client.connected()) {
                _cleanup_client(i, "disconnected");
                continue;
            }

            // Check read timeout (0 = disabled)
            if (_read_timeout > 0 &&
                _clients[i].last_activity > 0 &&
                (millis() - _clients[i].last_activity) > _read_timeout) {
                _cleanup_client(i, "read timeout");
                continue;
            }

            // Read available bytes and deframe
            while (_clients[i].client.available()) {
                uint8_t byte = _clients[i].client.read();
                _clients[i].last_activity = millis();
                _hdlc_deframe(i, byte);
            }
        }
    }

    // ─── Stats ───────────────────────────────────────────────────────────────
    int  clientCount() const { return _num_clients; }
    bool isStarted()   const { return _started; }
    bool isConnected() const { return _num_clients > 0; }
    void setReadTimeout(uint32_t timeout_ms) { _read_timeout = timeout_ms; }

protected:
    // ─── RNS InterfaceImpl: outgoing packet from RNS Transport ───────────────
    virtual void send_outgoing(const RNS::Bytes& data) override {
        if (!_started || _num_clients == 0) return;

        // HDLC frame the data
        uint8_t frame_buf[TCP_IF_HW_MTU * 2 + 4]; // worst case: every byte escaped + 2 flags
        uint16_t flen = 0;

        frame_buf[flen++] = HDLC_FLAG;
        for (size_t i = 0; i < data.size(); i++) {
            uint8_t b = data.data()[i];
            if (b == HDLC_FLAG || b == HDLC_ESC) {
                frame_buf[flen++] = HDLC_ESC;
                frame_buf[flen++] = b ^ HDLC_ESC_MASK;
            } else {
                frame_buf[flen++] = b;
            }
            if (flen >= sizeof(frame_buf) - 4) break; // safety
        }
        frame_buf[flen++] = HDLC_FLAG;

        // Send to all connected clients EXCEPT the one that sent this packet.
        // v1.0.10: Echo prevention — if this send_outgoing was triggered by
        // Transport forwarding a packet received from client N, skip client N
        // to prevent echo-back that floods TCP buffers and stalls resource transfers.
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (i == _last_rx_client_idx) {
                continue;  // Don't echo back to sender
            }
            if (_clients[i].active && _clients[i].client.connected()) {
                size_t written = _clients[i].client.write(frame_buf, flen);
                if (written == 0) {
                    _cleanup_client(i, "write failed");
                } else if (written < flen) {
                    Serial.printf("[TcpIF] PARTIAL write to client %d: %u/%u bytes\r\n", i, (unsigned)written, (unsigned)flen);
                }
            }
        }
        yield(); // feed WDT between TCP writes and RNS processing

        // Post-send housekeeping
        InterfaceImpl::handle_outgoing(data);
    }

    // ─── RNS InterfaceImpl: incoming packet to RNS Transport ─────────────────
    virtual void handle_incoming(const RNS::Bytes& data) override {
        TRACEF("TcpInterface.handle_incoming: (%u bytes)", data.size());
        InterfaceImpl::handle_incoming(data);
    }

private:
    // ─── Cleanup a client slot, freeing all lwIP resources ───────────────────
    void _cleanup_client(int idx, const char* reason) {
        TcpClient& c = _clients[idx];
        if (!c.active) return;

        uint32_t heap_before = ESP.getFreeHeap();

        // Set SO_LINGER with timeout 0: forces RST instead of FIN,
        // which skips TIME_WAIT and immediately frees the lwIP PCB
        // and all associated TCP send/receive buffers (~2-4 KB each).
        int fd = c.client.fd();
        if (fd >= 0) {
            struct linger lin;
            lin.l_onoff = 1;
            lin.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        }

        c.client.stop();
        c.client = WiFiClient();  // Release any residual shared_ptr state
        c.active = false;
        c.in_frame = false;
        c.escape = false;
        c.truncated = false;
        c.rxlen = 0;
        _num_clients--;

        uint32_t heap_after = ESP.getFreeHeap();
        Serial.printf("[TcpIF] Client %d %s (heap: %u -> %u, delta: %+d)\r\n",
                      idx, reason, heap_before, heap_after,
                      (int)(heap_after - heap_before));
    }

    // ─── HDLC byte-level deframing ──────────────────────────────────────────
    void _hdlc_deframe(int idx, uint8_t byte) {
        TcpClient& c = _clients[idx];

        if (byte == HDLC_FLAG) {
            if (c.in_frame && c.rxlen > 0) {
                // v1.0.12: If the frame exceeded the buffer, drop it entirely
                // instead of delivering a truncated/corrupt packet to Transport.
                if (c.truncated) {
                    Serial.printf("[TcpIF] DROPPED oversized frame from client %d (>%d bytes, buffered %u)\r\n",
                                  idx, TCP_IF_HW_MTU, c.rxlen);
                    c.truncated = false;
                    c.rxlen = 0;
                } else {
                    // End of frame — deliver to RNS
                    // v1.0.10: Set _last_rx_client_idx so send_outgoing() can
                    // skip echoing this packet back to the client that sent it.
                    // The entire call chain (handle_incoming → Transport::inbound
                    // → transmit → send_outgoing) is synchronous, so this is safe.
                    RNS::Bytes data(c.rxbuf, c.rxlen);
                    _last_rx_client_idx = idx;
                    handle_incoming(data);
                    _last_rx_client_idx = -1;
                    c.rxlen = 0;
                }
            }
            c.in_frame = true;
            c.escape = false;
            c.truncated = false;
            c.rxlen = 0;
        } else if (c.in_frame) {
            if (c.escape) {
                byte ^= HDLC_ESC_MASK;
                c.escape = false;
                if (c.rxlen < TCP_IF_HW_MTU) {
                    c.rxbuf[c.rxlen++] = byte;
                } else {
                    c.truncated = true;
                }
            } else if (byte == HDLC_ESC) {
                c.escape = true;
            } else {
                if (c.rxlen < TCP_IF_HW_MTU) {
                    c.rxbuf[c.rxlen++] = byte;
                } else {
                    c.truncated = true;
                }
            }
        }
    }

    // ─── Accept a new server-mode client ─────────────────────────────────────
    void _accept_client(WiFiClient& newClient) {
        // Find a free slot
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (!_clients[i].active) {
                // Defensive: force-release any residual lwIP resources in this slot
                // before assigning the new client (prevents PCB/buffer leaks)
                int fd = _clients[i].client.fd();
                if (fd >= 0) {
                    struct linger lin;
                    lin.l_onoff = 1;
                    lin.l_linger = 0;
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
                    _clients[i].client.stop();
                }
                _clients[i].client = WiFiClient();  // Reset to clean state

                _clients[i].client = newClient;
                _clients[i].client.setNoDelay(true);
                _clients[i].client.setTimeout(TCP_IF_WRITE_TIMEOUT / 1000);
                _clients[i].active = true;
                _clients[i].in_frame = false;
                _clients[i].escape = false;
                _clients[i].truncated = false;
                _clients[i].rxlen = 0;
                _clients[i].last_activity = millis();
                _num_clients++;
                Serial.printf("[TcpIF] Client %d connected from %s\r\n",
                              i, _clients[i].client.remoteIP().toString().c_str());
                return;
            }
        }
        // No free slots — reject
        Serial.println("[TcpIF] Max clients reached, rejecting connection");
        newClient.stop();
    }

    // ─── Client-mode outbound connection ─────────────────────────────────────
    void _connect_client() {
        if (_target_host[0] == '\0') {
            Serial.println("[TcpIF] No target host configured for client mode");
            return;
        }

        WiFiClient client;
        client.setTimeout(TCP_IF_CONNECT_TIMEOUT / 1000);

        bool connected = false;

        // Try cached IP first (avoids DNS lookup on every reconnect)
        if (_resolved_ip != (uint32_t)0) {
            Serial.printf("[TcpIF] Connecting to %s:%d (cached IP)...\r\n", _target_host, _target_port);
            connected = client.connect(_resolved_ip, _target_port);
            if (!connected) {
                // Cached IP failed — clear cache and try fresh DNS
                _resolved_ip = (uint32_t)0;
                Serial.println("[TcpIF] Cached IP failed, retrying with DNS");
            }
        }

        if (!connected) {
            Serial.printf("[TcpIF] Connecting to %s:%d (DNS)...\r\n", _target_host, _target_port);
            IPAddress resolved;
            if (WiFi.hostByName(_target_host, resolved)) {
                _resolved_ip = resolved;
                Serial.printf("[TcpIF] Resolved %s -> %s\r\n", _target_host, resolved.toString().c_str());
                connected = client.connect(resolved, _target_port);
            } else {
                Serial.printf("[TcpIF] DNS failed for %s\r\n", _target_host);
            }
        }

        if (connected) {
            client.setNoDelay(true);
            client.setTimeout(TCP_IF_WRITE_TIMEOUT / 1000);
            _clients[0].client = client;
            _clients[0].active = true;
            _clients[0].in_frame = false;
            _clients[0].escape = false;
            _clients[0].truncated = false;
            _clients[0].rxlen = 0;
            _clients[0].last_activity = millis();
            _num_clients = 1;
            _consecutive_failures = 0;
            _reconnect_interval = TCP_IF_RECONNECT_MIN;
            Serial.printf("[TcpIF] Connected to backbone at %s:%d\r\n",
                          _target_host, _target_port);
        } else {
            _consecutive_failures++;
            // Exponential backoff: 10s -> 20s -> 40s -> 80s -> 120s (max)
            _reconnect_interval = _reconnect_interval * 2;
            if (_reconnect_interval > TCP_IF_RECONNECT_MAX) {
                _reconnect_interval = TCP_IF_RECONNECT_MAX;
            }
            Serial.printf("[TcpIF] Failed to connect to %s:%d (attempt %d, next retry in %ds)\r\n",
                          _target_host, _target_port, _consecutive_failures,
                          _reconnect_interval / 1000);
        }
        _last_reconnect = millis();
    }

    // ─── Member variables ────────────────────────────────────────────────────
    TcpIfMode   _mode;
    uint16_t    _port;
    char        _target_host[64];
    uint16_t    _target_port;
    WiFiServer* _server;
    TcpClient   _clients[TCP_IF_MAX_CLIENTS];
    int         _num_clients;
    uint32_t    _last_reconnect;
    uint32_t    _last_keepalive;
    uint32_t    _reconnect_interval;
    uint32_t    _read_timeout;
    IPAddress   _resolved_ip;
    uint16_t    _consecutive_failures;
    bool        _started;
    int         _last_rx_client_idx = -1;  // v1.0.10: echo prevention — tracks which client is currently delivering an inbound frame
};

#endif // BOUNDARY_MODE
#endif // HAS_RNS
#endif // TCP_INTERFACE_H
