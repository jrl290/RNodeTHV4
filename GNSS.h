// GNSS.h — UC6580 GNSS driver for Heltec Wireless Tracker v1.1
// Phase 1: read NMEA from UART2, parse into gnss_state, log to Serial.
// Phase 2 (future): feed lat/lon into Advertise.h position beacons.

#pragma once

#if HAS_GNSS

#include <Arduino.h>

// ── State ───────────────────────────────────────────────────────────────────

struct GnssState {
    bool    fix;
    uint8_t satellites;
    double  latitude;   // degrees, positive = North
    double  longitude;  // degrees, positive = East
    float   altitude;   // metres MSL
    uint8_t hour, minute, second;
    bool    ready;      // true after first valid fix
};

GnssState gnss_state = {};

// ── Internal parser state ────────────────────────────────────────────────────

#define GNSS_UART       Serial2
#define GNSS_BAUD       115200
#define GNSS_BUF_SIZE   128

static char  _gnss_buf[GNSS_BUF_SIZE];
static uint8_t _gnss_buf_pos = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

static double _parse_nmea_coord(const char *field, const char *hemi) {
    // NMEA format: DDDMM.MMMM or DDMM.MMMM
    if (!field || field[0] == '\0') return 0.0;
    double raw  = atof(field);
    int    deg  = (int)(raw / 100);
    double mins = raw - deg * 100.0;
    double val  = deg + mins / 60.0;
    if (hemi && (hemi[0] == 'S' || hemi[0] == 'W')) val = -val;
    return val;
}

static uint8_t _tok(const char *sentence, uint8_t idx, char *out, uint8_t maxlen) {
    const char *p = sentence;
    uint8_t cur = 0;
    while (*p && cur < idx) { if (*p++ == ',') cur++; }
    uint8_t n = 0;
    while (*p && *p != ',' && *p != '*' && n < maxlen - 1) out[n++] = *p++;
    out[n] = '\0';
    return n;
}

// Parse $GNGGA or $GPGGA: time, lat, lon, fix, sats, alt
static void _parse_gga(const char *s) {
    char f[16];

    _tok(s, 1, f, sizeof(f));
    if (strlen(f) >= 6) {
        gnss_state.hour   = (f[0]-'0')*10 + (f[1]-'0');
        gnss_state.minute = (f[2]-'0')*10 + (f[3]-'0');
        gnss_state.second = (f[4]-'0')*10 + (f[5]-'0');
    }

    char lat[12], latH[2], lon[12], lonH[2];
    _tok(s, 2, lat,  sizeof(lat));
    _tok(s, 3, latH, sizeof(latH));
    _tok(s, 4, lon,  sizeof(lon));
    _tok(s, 5, lonH, sizeof(lonH));

    char fixQ[2], sats[4], alt[12];
    _tok(s, 6, fixQ, sizeof(fixQ));
    _tok(s, 7, sats, sizeof(sats));
    _tok(s, 9, alt,  sizeof(alt));

    gnss_state.fix        = (fixQ[0] >= '1');
    gnss_state.satellites = (uint8_t)atoi(sats);
    gnss_state.latitude   = _parse_nmea_coord(lat, latH);
    gnss_state.longitude  = _parse_nmea_coord(lon, lonH);
    gnss_state.altitude   = atof(alt);

    if (gnss_state.fix) gnss_state.ready = true;
}

static void _process_sentence(const char *s) {
    // Only handle GGA for now (position + fix quality)
    if (strncmp(s + 1, "GNGGA", 5) == 0 || strncmp(s + 1, "GPGGA", 5) == 0) {
        _parse_gga(s);
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

void gnss_init() {
    // Hard-reset UC6580 then release
    pinMode(TRACKER_GNSS_RST, OUTPUT);
    digitalWrite(TRACKER_GNSS_RST, LOW);
    delay(10);
    digitalWrite(TRACKER_GNSS_RST, HIGH);
    delay(100);

    GNSS_UART.begin(GNSS_BAUD, SERIAL_8N1, TRACKER_GNSS_RX, TRACKER_GNSS_TX);
}

// Call from the main loop; non-blocking, accumulates characters.
void gnss_update() {
    while (GNSS_UART.available()) {
        char c = (char)GNSS_UART.read();
        if (c == '$') {
            _gnss_buf_pos = 0;
        }
        if (_gnss_buf_pos < GNSS_BUF_SIZE - 1) {
            _gnss_buf[_gnss_buf_pos++] = c;
        }
        if (c == '\n' && _gnss_buf_pos > 6) {
            _gnss_buf[_gnss_buf_pos] = '\0';
            _process_sentence(_gnss_buf);
            _gnss_buf_pos = 0;
        }
    }
}

#endif // HAS_GNSS
