// ─────────────────────────────────────────────────────────────────────────────
//  Advertise.h — Reticulum interface-discovery announcer for RTNode
//
//  Implements the on-network interface-discovery announce protocol described
//  in the Reticulum manual: https://reticulum.network/manual/interfaces.html
//  Compatible with RNS/Discovery.py (InterfaceAnnouncer / InterfaceAnnounceHandler)
//  and LXMF/LXStamper.py (stamp generation).
//
//  When BOUNDARY_MODE is active and the user has enabled "Advertise Device" in
//  the captive-portal configuration, this module periodically sends an
//  RNS announce on a destination with aspects "rnstransport.discovery.interface"
//  whose app_data is:
//
//      bytes([flags]) || msgpack(info_dict) || stamp
//
//  where info_dict contains the documented byte-id fields (interface type,
//  transport ID, latitude/longitude/height, LoRa parameters, etc.) and stamp
//  is an LXMF proof-of-work over a SHA-256/HKDF-SHA256 workblock.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef ADVERTISE_H
#define ADVERTISE_H

#ifdef BOUNDARY_MODE

#include <Arduino.h>
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Reticulum.h>
#include <Cryptography/HKDF.h>
#include <Log.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "BoundaryMode.h"

#if defined(ESP32)
#include <esp_task_wdt.h>
#endif

// Externally-defined LoRa parameters (see Config.h / RNode_Firmware.ino)
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;

// Cached node-hash hex string in RTC memory (see RNode_Firmware.ino).
#ifndef NODE_HASH_RTC_MAGIC
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
#endif
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];

// ─── Protocol constants (must match RNS/Discovery.py & LXMF/LXStamper.py) ───
#define ADV_FIELD_INTERFACE_TYPE  0x00
#define ADV_FIELD_TRANSPORT       0x01
#define ADV_FIELD_REACHABLE_ON    0x02
#define ADV_FIELD_LATITUDE        0x03
#define ADV_FIELD_LONGITUDE       0x04
#define ADV_FIELD_HEIGHT          0x05
#define ADV_FIELD_PORT            0x06
#define ADV_FIELD_IFAC_NETNAME    0x07
#define ADV_FIELD_IFAC_NETKEY     0x08
#define ADV_FIELD_FREQUENCY       0x09
#define ADV_FIELD_BANDWIDTH       0x0A
#define ADV_FIELD_SPREADINGFACTOR 0x0B
#define ADV_FIELD_CODINGRATE      0x0C
#define ADV_FIELD_MODULATION      0x0D
#define ADV_FIELD_CHANNEL         0x0E
#define ADV_FIELD_TRANSPORT_ID    0xFE
#define ADV_FIELD_NAME            0xFF

#define ADV_FLAG_SIGNED    0x01
#define ADV_FLAG_ENCRYPTED 0x02

// Defaults — must match Reticulum's interface-announcer defaults so the
// stamp validates against the on-network handler (RNS/Discovery.py).
#define ADV_DEFAULT_STAMP_COST      14
#define ADV_WORKBLOCK_EXPAND_ROUNDS 20
#define ADV_STAMP_SIZE              32 /* SHA-256 / HASHLENGTH/8 */

// Default announce interval matches RNS Reticulum.py's discoverable-interface
// fallback when announce_interval is not specified (6 hours). LoRa airtime
// is precious; this is intentionally conservative.
#define ADV_DEFAULT_ANNOUNCE_INTERVAL_S (6UL * 60UL * 60UL)
// Initial delay after boot before the first announce — gives the radio,
// transport and any TCP backbone time to come up.
#define ADV_INITIAL_DELAY_MS            (60UL * 1000UL)

// Privacy jitter radius. ~half a kilometre / half a mile.
#define ADV_JITTER_RADIUS_METERS 800.0

// ─── Module state ────────────────────────────────────────────────────────────
static RNS::Destination advertise_destination = {RNS::Type::NONE};
static bool     advertise_initialised      = false;
static bool     advertise_first_announce   = true;
static uint32_t advertise_next_run_ms      = 0;
static uint32_t advertise_announce_interval_ms = ADV_DEFAULT_ANNOUNCE_INTERVAL_S * 1000UL;

// Cached stamp keyed by infohash so we only redo the proof-of-work when the
// advertised parameters actually change (matches InterfaceAnnouncer.stamp_cache).
static RNS::Bytes advertise_cached_infohash;
static RNS::Bytes advertise_cached_stamp;

// ─── MessagePack encoder ────────────────────────────────────────────────────
// Minimal encoder covering the types required by the discovery info dict:
// fixmap/map16, fixstr/str8/str16, bin8/16, bool, float64, uint8/16/32, fixint.
// Output is appended to a RNS::Bytes buffer.

static inline void adv_mp_byte(RNS::Bytes& out, uint8_t b) { out.append(b); }

static inline void adv_mp_bytes(RNS::Bytes& out, const uint8_t* p, size_t n) {
    out.append(p, n);
}

static inline void adv_mp_be16(RNS::Bytes& out, uint16_t v) {
    adv_mp_byte(out, (v >> 8) & 0xFF);
    adv_mp_byte(out, v & 0xFF);
}

static inline void adv_mp_be32(RNS::Bytes& out, uint32_t v) {
    adv_mp_byte(out, (v >> 24) & 0xFF);
    adv_mp_byte(out, (v >> 16) & 0xFF);
    adv_mp_byte(out, (v >> 8)  & 0xFF);
    adv_mp_byte(out, v & 0xFF);
}

static inline void adv_mp_map_header(RNS::Bytes& out, uint32_t n) {
    if (n <= 15) {
        adv_mp_byte(out, 0x80 | (uint8_t)n);
    } else if (n <= 0xFFFF) {
        adv_mp_byte(out, 0xde);
        adv_mp_be16(out, (uint16_t)n);
    } else {
        adv_mp_byte(out, 0xdf);
        adv_mp_be32(out, n);
    }
}

static inline void adv_mp_uint(RNS::Bytes& out, uint64_t v) {
    if (v <= 0x7F) {
        adv_mp_byte(out, (uint8_t)v);
    } else if (v <= 0xFF) {
        adv_mp_byte(out, 0xcc);
        adv_mp_byte(out, (uint8_t)v);
    } else if (v <= 0xFFFF) {
        adv_mp_byte(out, 0xcd);
        adv_mp_be16(out, (uint16_t)v);
    } else if (v <= 0xFFFFFFFFULL) {
        adv_mp_byte(out, 0xce);
        adv_mp_be32(out, (uint32_t)v);
    } else {
        adv_mp_byte(out, 0xcf);
        for (int i = 7; i >= 0; --i) adv_mp_byte(out, (uint8_t)(v >> (i * 8)));
    }
}

static inline void adv_mp_bool(RNS::Bytes& out, bool v) {
    adv_mp_byte(out, v ? 0xc3 : 0xc2);
}

static inline void adv_mp_str(RNS::Bytes& out, const char* s) {
    size_t len = (s == nullptr) ? 0 : strlen(s);
    if (len <= 31) {
        adv_mp_byte(out, 0xa0 | (uint8_t)len);
    } else if (len <= 0xFF) {
        adv_mp_byte(out, 0xd9);
        adv_mp_byte(out, (uint8_t)len);
    } else if (len <= 0xFFFF) {
        adv_mp_byte(out, 0xda);
        adv_mp_be16(out, (uint16_t)len);
    } else {
        adv_mp_byte(out, 0xdb);
        adv_mp_be32(out, (uint32_t)len);
    }
    if (len > 0) adv_mp_bytes(out, (const uint8_t*)s, len);
}

static inline void adv_mp_bin(RNS::Bytes& out, const uint8_t* p, size_t len) {
    if (len <= 0xFF) {
        adv_mp_byte(out, 0xc4);
        adv_mp_byte(out, (uint8_t)len);
    } else if (len <= 0xFFFF) {
        adv_mp_byte(out, 0xc5);
        adv_mp_be16(out, (uint16_t)len);
    } else {
        adv_mp_byte(out, 0xc6);
        adv_mp_be32(out, (uint32_t)len);
    }
    if (len > 0) adv_mp_bytes(out, p, len);
}

// IEEE-754 binary64, big-endian (msgpack float64, prefix 0xcb).
static inline void adv_mp_float64(RNS::Bytes& out, double v) {
    static_assert(sizeof(double) == 8, "msgpack float64 requires IEEE-754 binary64");
    uint8_t buf[8];
    memcpy(buf, &v, 8);
    // Detect host endianness — virtually always little-endian on ESP32.
    const uint16_t endian_test = 1;
    bool host_little = (*reinterpret_cast<const uint8_t*>(&endian_test)) == 1;
    adv_mp_byte(out, 0xcb);
    if (host_little) {
        for (int i = 7; i >= 0; --i) adv_mp_byte(out, buf[i]);
    } else {
        adv_mp_bytes(out, buf, 8);
    }
}

// Pack a small integer key (the field IDs are all <= 0xFF). Values <=127 fit
// as a positive fixint; 0xFE and 0xFF need a uint8 prefix.
static inline void adv_mp_key(RNS::Bytes& out, uint8_t key) {
    adv_mp_uint(out, (uint64_t)key);
}

// ─── Privacy jitter (deterministic per-device) ───────────────────────────────
// Compute a stable lat/lon offset of up to ADV_JITTER_RADIUS_METERS using the
// node's destination hash as the seed. Stable per device → the pin doesn't
// move around between announces, but its precise location is obscured.
static void advertise_apply_jitter(double& lat, double& lon, const RNS::Bytes& seed_hash) {
    if (seed_hash.size() < 8) return;
    const uint8_t* h = seed_hash.data();

    // Two independent uniform [0, 1) values from the hash bytes.
    uint32_t a = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8)  |  (uint32_t)h[3];
    uint32_t b = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
                 ((uint32_t)h[6] << 8)  |  (uint32_t)h[7];
    double u1 = (double)a / 4294967296.0;
    double u2 = (double)b / 4294967296.0;

    // Uniform-disk sampling to avoid bunching at the centre.
    double r     = sqrt(u1) * ADV_JITTER_RADIUS_METERS;
    double theta = 2.0 * M_PI * u2;

    // 1 deg latitude ≈ 111 320 m. 1 deg longitude ≈ 111 320 m * cos(lat).
    double dlat = (r * cos(theta)) / 111320.0;
    double cos_lat = cos(lat * M_PI / 180.0);
    if (cos_lat < 1e-6) cos_lat = 1e-6; // guard against the poles
    double dlon = (r * sin(theta)) / (111320.0 * cos_lat);

    lat += dlat;
    lon += dlon;

    // Clamp back into valid ranges in the unlikely event of a pole-adjacent input.
    if (lat >  90.0) lat =  90.0;
    if (lat < -90.0) lat = -90.0;
    if (lon >  180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
}

// ─── LXMF stamp (proof-of-work) ──────────────────────────────────────────────
// Generates an HKDF-SHA256 workblock and finds a 32-byte stamp such that
// SHA-256(workblock || stamp) interpreted as a big-endian integer is no
// greater than (1 << (256 - cost)). Matches LXMF/LXStamper.py.

static RNS::Bytes advertise_stamp_workblock(const RNS::Bytes& material) {
    RNS::Bytes workblock;
    for (int n = 0; n < ADV_WORKBLOCK_EXPAND_ROUNDS; ++n) {
        // salt = full_hash(material || msgpack(n))
        RNS::Bytes salt_input;
        salt_input.append(material);
        adv_mp_uint(salt_input, (uint64_t)n);
        RNS::Bytes salt = RNS::Identity::full_hash(salt_input);

        RNS::Bytes round = RNS::Cryptography::hkdf(256, material, salt);
        workblock.append(round);

#if defined(ESP32)
        esp_task_wdt_reset();
#endif
    }
    return workblock;
}

// Returns true and writes a valid stamp into "stamp_out" on success.
static bool advertise_generate_stamp(const RNS::Bytes& workblock,
                                     uint8_t cost,
                                     RNS::Bytes& stamp_out) {
    if (cost == 0 || cost > 32) return false;

    // target = 1 << (256 - cost). We compare the leading bytes of the SHA-256
    // result against this threshold by counting leading zero bits.
    const uint32_t leading_zero_bits_required = cost;

    uint32_t round = 0;
    while (true) {
        RNS::Bytes candidate = RNS::Cryptography::random(ADV_STAMP_SIZE);

        RNS::Bytes hash_input;
        hash_input.append(workblock);
        hash_input.append(candidate);
        RNS::Bytes h = RNS::Identity::full_hash(hash_input);

        // Count leading zero bits.
        uint32_t lz = 0;
        const uint8_t* hp = h.data();
        size_t hsize = h.size();
        for (size_t i = 0; i < hsize && lz < leading_zero_bits_required; ++i) {
            uint8_t byte = hp[i];
            if (byte == 0) {
                lz += 8;
                continue;
            }
            for (int bit = 7; bit >= 0; --bit) {
                if ((byte >> bit) & 1) goto stamp_count_done;
                lz++;
            }
        }
        stamp_count_done:
        if (lz >= leading_zero_bits_required) {
            stamp_out = candidate;
            return true;
        }

        if ((++round & 0x3FF) == 0) { // every 1024 attempts
#if defined(ESP32)
            esp_task_wdt_reset();
#endif
            // Hard cap to avoid pathological infinite loops on misconfiguration.
            if (round > (1UL << (cost + 6))) {
                return false;
            }
        }
    }
}

// ─── Build the discovery info map ────────────────────────────────────────────
// Packs the info dict per RNS/Discovery.py::get_interface_announce_data()
// for INTERFACE_TYPE = "RNodeInterface".
static RNS::Bytes advertise_build_info() {
    RNS::Bytes packed;

    // Determine which optional fields will be included so we can write a
    // correct map header up front.
    bool include_ifac =
        boundary_state.ifac_enabled &&
        (boundary_state.ifac_netname[0] != '\0' ||
         boundary_state.ifac_passphrase[0] != '\0');

    // Required keys: INTERFACE_TYPE, TRANSPORT, TRANSPORT_ID, NAME,
    //                LATITUDE, LONGITUDE, HEIGHT,
    //                FREQUENCY, BANDWIDTH, SPREADINGFACTOR, CODINGRATE
    // Optional keys: IFAC_NETNAME, IFAC_NETKEY (if publish_ifac equivalent)
    uint32_t map_entries = 11;
    if (include_ifac) map_entries += 2;

    adv_mp_map_header(packed, map_entries);

    // INTERFACE_TYPE = "RNodeInterface"
    adv_mp_key(packed, ADV_FIELD_INTERFACE_TYPE);
    adv_mp_str(packed, "RNodeInterface");

    // TRANSPORT (bool) — whether transport is enabled on this node
    adv_mp_key(packed, ADV_FIELD_TRANSPORT);
    adv_mp_bool(packed, RNS::Reticulum::transport_enabled());

    // TRANSPORT_ID (bin) — RNS::Transport identity hash (truncated, 16 bytes)
    {
        const RNS::Bytes& tid_hash = RNS::Transport::identity().hash();
        adv_mp_key(packed, ADV_FIELD_TRANSPORT_ID);
        adv_mp_bin(packed, tid_hash.data(), tid_hash.size());
    }

    // NAME (str) — discovery_name. Use the cached node hash hex prefix when no
    // explicit name has been configured, so each node has a unique identifier
    // visible on maps.
    {
        char name_buf[40];
        const char* hex = (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0')
                          ? rtc_node_hash_hex : "";
        snprintf(name_buf, sizeof(name_buf), "RTNode-%.8s", hex[0] ? hex : "unknown");
        adv_mp_key(packed, ADV_FIELD_NAME);
        adv_mp_str(packed, name_buf);
    }

    // LATITUDE / LONGITUDE (float64) — apply optional privacy jitter.
    {
        double adv_lat = boundary_state.advert_lat;
        double adv_lon = boundary_state.advert_lon;
        if (boundary_state.advert_jitter && advertise_destination) {
            advertise_apply_jitter(adv_lat, adv_lon, advertise_destination.hash());
        }
        adv_mp_key(packed, ADV_FIELD_LATITUDE);
        adv_mp_float64(packed, adv_lat);
        adv_mp_key(packed, ADV_FIELD_LONGITUDE);
        adv_mp_float64(packed, adv_lon);
    }

    // HEIGHT (float64) — not configurable yet; default to 0 metres.
    adv_mp_key(packed, ADV_FIELD_HEIGHT);
    adv_mp_float64(packed, 0.0);

    // RNodeInterface-specific radio parameters (per Discovery.py:144-148).
    adv_mp_key(packed, ADV_FIELD_FREQUENCY);
    adv_mp_uint(packed, (uint64_t)lora_freq);
    adv_mp_key(packed, ADV_FIELD_BANDWIDTH);
    adv_mp_uint(packed, (uint64_t)lora_bw);
    adv_mp_key(packed, ADV_FIELD_SPREADINGFACTOR);
    adv_mp_uint(packed, (uint64_t)lora_sf);
    adv_mp_key(packed, ADV_FIELD_CODINGRATE);
    adv_mp_uint(packed, (uint64_t)lora_cr);

    if (include_ifac) {
        adv_mp_key(packed, ADV_FIELD_IFAC_NETNAME);
        adv_mp_str(packed, boundary_state.ifac_netname);
        adv_mp_key(packed, ADV_FIELD_IFAC_NETKEY);
        adv_mp_str(packed, boundary_state.ifac_passphrase);
    }

    return packed;
}

// ─── Send a single discovery announce ───────────────────────────────────────
static void advertise_send_announce() {
    if (!advertise_destination) return;

    RNS::verbose("[Advertise] Building discovery announce");
    RNS::Bytes packed = advertise_build_info();
    RNS::Bytes infohash = RNS::Identity::full_hash(packed);

    RNS::Bytes stamp;
    bool need_pow = true;
    if (advertise_cached_infohash.size() > 0 &&
        advertise_cached_infohash == infohash &&
        advertise_cached_stamp.size() == ADV_STAMP_SIZE) {
        stamp = advertise_cached_stamp;
        need_pow = false;
        RNS::verbose("[Advertise] Reusing cached stamp (info unchanged)");
    }

    if (need_pow) {
        RNS::verbose("[Advertise] Generating workblock + stamp (cost=14, this may take a few seconds)");
        RNS::Bytes workblock = advertise_stamp_workblock(infohash);
        if (!advertise_generate_stamp(workblock, ADV_DEFAULT_STAMP_COST, stamp)) {
            RNS::error("[Advertise] Stamp generation failed; skipping announce");
            return;
        }
        advertise_cached_infohash = infohash;
        advertise_cached_stamp    = stamp;
    }

    // Assemble payload: bytes([flags]) || packed || stamp
    RNS::Bytes app_data;
    app_data.append((uint8_t)0x00); // flags: not signed, not encrypted
    app_data.append(packed);
    app_data.append(stamp);

    RNS::verbose("[Advertise] Sending interface discovery announce, payload size: " +
                 std::to_string((int)app_data.size()) + " bytes");
    advertise_destination.announce(app_data);
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Initialise the advertise destination. Call once after RNS has been started
// and Transport::identity() is available. Safe to call multiple times — only
// the first call has any effect.
inline void advertise_init() {
    if (advertise_initialised) return;
    if (!RNS::Transport::identity()) return;

    // Reticulum's discovery destination uses the network identity when one is
    // configured, otherwise the transport identity. We have no concept of a
    // separate network identity in the firmware, so use the transport identity.
    advertise_destination = RNS::Destination(
        RNS::Transport::identity(),
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "rnstransport",
        "discovery.interface"
    );

    advertise_initialised    = true;
    advertise_first_announce = true;
    advertise_next_run_ms    = millis() + ADV_INITIAL_DELAY_MS;
    advertise_announce_interval_ms = ADV_DEFAULT_ANNOUNCE_INTERVAL_S * 1000UL;

    if (boundary_state.advert_enabled) {
        RNS::info("[Advertise] Device advertisement ENABLED — first announce in ~" +
                  std::to_string(ADV_INITIAL_DELAY_MS / 1000) + "s");
    } else {
        RNS::verbose("[Advertise] Device advertisement disabled (configure in portal to enable)");
    }
}

// Periodic loop hook — call from the main loop().
inline void advertise_loop() {
    if (!advertise_initialised) return;
    if (!boundary_state.advert_enabled) return;

    uint32_t now = millis();
    // Handle uint32 wrap-around: only treat as "due" when the unsigned
    // difference is small. millis() wraps roughly every 49 days, well after
    // any reasonable announce interval, so a wrap will at worst cause a single
    // announce to fire one cycle early.
    int32_t delta = (int32_t)(now - advertise_next_run_ms);
    if (delta < 0) return;

    advertise_send_announce();

    advertise_first_announce = false;
    advertise_next_run_ms    = now + advertise_announce_interval_ms;
}

#endif // BOUNDARY_MODE
#endif // ADVERTISE_H
