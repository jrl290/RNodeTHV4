# microReticulum Bug Report

## 1. Copy-vs-Reference Bugs in Transport.cpp

### Summary

Several locations in `Transport.cpp` retrieve `LinkEntry` or `DestinationEntry` values from `std::map` containers **by copy** instead of **by reference**. Subsequent mutations (setting `_validated`, updating `_timestamp`) only affect the local copy — the actual map entry is never updated.

This is a Python→C++ porting issue: in Python, `dict[key]` returns a reference to the value, so `transport.link_table[link_id][0] = time.time()` mutates the dict entry in place. In C++, `(*iter).second` returns a copy when assigned to a non-reference variable.

### Bug 1a — LRPROOF handler: `_validated` never set on map entry (CRITICAL)

**File:** `Transport.cpp`, LRPROOF handling section  
**Code (before fix):**
```cpp
LinkEntry link_entry = (*_link_table.find(packet.destination_hash())).second;  // COPY
// ... later:
link_entry._validated = true;  // Only updates local copy
```

**Impact:** The link_table entry's `_validated` stays `false`. The culling code in `jobs()` checks:
```cpp
if (link_entry._validated) {
    if (OS::time() > (link_entry._timestamp + LINK_TIMEOUT)) { ... }  // 15 minutes
} else {
    if (OS::time() > link_entry._proof_timeout) { ... }               // 6-18 seconds
}
```
Since `_validated` is never set to `true`, the entry is culled at `proof_timeout` (6 × hops = **6–18 seconds**) instead of `LINK_TIMEOUT` (15 minutes). All subsequent link data packets (resource chunks, keepalives, etc.) are silently dropped once the entry is removed.

**Symptom:** Resource transfers start successfully but fail after a few seconds. Link establishes, some data transfers, then all traffic stops. Affects both directions.

**Fix:** Change to reference:
```cpp
LinkEntry& link_entry = (*_link_table.find(packet.destination_hash())).second;
```

### Bug 1b — Link transport handler: `_timestamp` never refreshed (CRITICAL)

**File:** `Transport.cpp`, link transport handling section  
**Code (before fix):**
```cpp
LinkEntry link_entry = (*link_iter).second;  // COPY
// ... later:
link_entry._timestamp = OS::time();  // Only updates local copy
```

**Impact:** Even with Bug 1a fixed, the link_table entry's `_timestamp` is set once (at LINKREQUEST time) and never refreshed during ongoing link data forwarding. For long-running transfers exceeding `LINK_TIMEOUT` (KEEPALIVE=360s, STALE_TIME=720s, LINK_TIMEOUT=900s = 15 minutes), the entry is eventually culled and the transfer fails.

**Fix:** Change to reference:
```cpp
LinkEntry& link_entry = (*link_iter).second;
```

### Bug 1c — Standard inbound transport forwarding: `_timestamp` never refreshed

**File:** `Transport.cpp`, inbound transport forwarding (HEADER_2 packets where we are the designated next-hop)  
**Code (before fix):**
```cpp
DestinationEntry destination_entry = (*destination_iter).second;  // COPY
// ... later:
destination_entry._timestamp = OS::time();  // Only updates local copy
```

**Impact:** Path timestamps are never refreshed when packets are actively being forwarded along that path. Paths could be culled while still in active use, though the timeout is typically long enough (DESTINATION_TIMEOUT) that this is less likely to cause issues than the link_table bugs.

**Fix:** Change to reference:
```cpp
DestinationEntry& destination_entry = (*destination_iter).second;
```

### Bug 1d — Outbound transport forwarding: `_timestamp` never refreshed

**File:** `Transport.cpp`, `outbound()` method  
**Code (before fix):**
```cpp
DestinationEntry destination_entry = (*_destination_table.find(packet.destination_hash())).second;  // COPY
// ... later:
destination_entry._timestamp = OS::time();  // Only updates local copy
```

**Impact:** Same as 1c — outbound path forwarding never refreshes the path timestamp.

**Fix:** Change to reference:
```cpp
DestinationEntry& destination_entry = (*_destination_table.find(packet.destination_hash())).second;
```

### How to audit for more

Search for the pattern: assignments from map iterators without `&`:
```
grep -n "Entry [a-z_]* = " Transport.cpp
```
Any line matching `SomeEntry variable_name = (*iter).second;` (without `&` after the type) that later mutates a field of `variable_name` is a bug.

---

## 2. Potential additional copy-vs-reference instances (non-mutating — safe but wasteful)

The following locations also copy entries but only read from them (no mutation). They are not bugs but are unnecessarily copying large structs:

- `for_local_client` check: `LinkEntry link_entry = (*link_iter).second;` — read-only, safe
- `for_local_client` check: `DestinationEntry destination_entry = (*destination_iter).second;` — read-only, safe
- `proof_for_local_client` check: `ReverseEntry reverse_entry = (*reverse_iter).second;` — read-only, safe
- Several `DestinationEntry` copies in `has_path()`, `hops_to()`, `next_hop()`, `next_hop_interface()` — read-only, safe

These could be changed to `const auto&` for efficiency but are not correctness bugs.

---

## 3. `std::map::insert()` silently fails on existing keys

### Summary

In Python, `dict[key] = value` always overwrites. In C++, `std::map::insert({key, value})` is a **no-op** if the key already exists — it silently discards the new value and returns the old entry.

### Bug 3a — `_destination_table` path updates silently ignored

**File:** `Transport.cpp`, path table update code  
**Code (before fix):**
```cpp
_destination_table.insert({destination_hash, new_destination_entry});
// If destination_hash already exists, the insert does NOTHING.
// The old (stale) path entry remains.
```

**Impact:** When a destination announces a new path (e.g. it roamed to a different transport node), the path table keeps the old stale entry. Packets continue to be routed to the old path, which may no longer work.

**Fix:** Erase before insert:
```cpp
_destination_table.erase(destination_hash);
_destination_table.insert({destination_hash, new_destination_entry});
```

**Note:** This same pattern (`insert` without `erase`) should be audited across ALL map insertions in the codebase. Any map that might receive updated entries for existing keys needs the erase-before-insert pattern, or should use `operator[]` or `insert_or_assign()`.

---

## 4. Memory Leaks — Unbounded Data Structures

### Bug 4a — `_boundary_local_addresses`: no cap, no eviction (HIGH RISK)

**File:** `Transport.cpp`

The `_boundary_local_addresses` set accumulates every local device address seen via LoRa announces. There is no size cap and no eviction mechanism. On a long-running boundary node that sees many transient devices, this grows without bound.

**Impact:** Slow heap exhaustion over days/weeks of operation. Particularly problematic on ESP32 with limited RAM.

**Fix needed:** Add a size cap (e.g. 200) with timestamp-based or LRU eviction, similar to how `_boundary_mentioned_addresses` is capped.

### Bug 4b — `_held_announces`: no cap, can orphan

**File:** `Transport.cpp`

The `_held_announces` map stores announces waiting to be retransmitted. There is no size cap. If the retransmit never triggers (e.g. the outbound interface disappears), entries can be orphaned and accumulate indefinitely.

**Impact:** Slow memory leak, exacerbated on busy networks with many announces.

**Fix needed:** Add a size cap or timeout-based eviction.

### Bug 4c — `_pending_local_path_requests`: entries never erased

**File:** `Transport.cpp`  
**Status:** Fixed (added `.erase(iter)` call)

Entries in `_pending_local_path_requests` were inserted but never removed after the path request was fulfilled. Over time the map grew without bound.

### Bug 4d — `_path_requests`: entries never culled

**File:** `Transport.cpp`  
**Status:** Fixed (added `DESTINATION_TIMEOUT`-based culling in `jobs()`)

The `_path_requests` map recorded timestamps of path requests but entries were never removed. Each unique destination hash that triggered a path request stayed in the map forever.

---

## 5. Spurious Path Request Broadcasts

### Bug 5a — Boundary Path A sends PATH REQUEST for every link data packet

**File:** `Transport.cpp`, boundary mode local→backbone forwarding  
**Code (before fix):**
```cpp
// Boundary Path A: local device packet, no path in _destination_table
else {
    DEBUG("BOUNDARY: No path to " + packet.destination_hash().toHex() + " for local packet. Requesting path.");
    request_path(packet.destination_hash());
}
```

**Impact:** Link data packets are addressed to a `link_id` (the link's unique identifier), not a destination hash. The `link_id` will never be found in `_destination_table` — it's only in `_link_table`. So **every** link data packet from a local device triggers a `request_path(link_id)` call, which broadcasts a PATH REQUEST for a hash that is not any destination.

For a resource transfer with 100 chunks, this sends 100 useless PATH REQUEST broadcasts over LoRa (with no deduplication — `request_path()` always sends). This wastes radio airtime and can cause congestion-related timeouts on slow LoRa links.

**Fix:** Check if the destination is a known link_id before requesting a path:
```cpp
else {
    if (_link_table.find(packet.destination_hash()) == _link_table.end()) {
        DEBUG("BOUNDARY: No path to " + packet.destination_hash().toHex() + " for local packet. Requesting path.");
        request_path(packet.destination_hash());
    }
}
```

### Bug 5b — Same issue in standard transport HEADER_2 fallback path

**File:** `Transport.cpp`, inbound transport forwarding (where we are designated next-hop but destination_table lookup fails)

Same pattern: for link data packets with HEADER_2/TRANSPORT headers where the transport_id matches us, if the destination_hash (which is a link_id) isn't in the destination table, the code requests a path for the link_id — another spurious broadcast.

**Fix:** Same guard — check `_link_table` before calling `request_path()`.

---

## 6. General Audit Recommendations

### 6a — Systematic `insert()` audit

Every `std::map::insert()` call in the codebase should be reviewed. The ones that are intentional "insert-if-not-exists" semantics are fine. The ones ported from Python `dict[key] = value` (which overwrites) need to use erase+insert or `insert_or_assign()`.

### 6b — Systematic copy-vs-reference audit

Run:
```bash
grep -n "Entry [a-z_]* = .*\.second" Transport.cpp
```
Any match where the variable is later mutated (assigned to `._timestamp`, `._validated`, etc.) is a bug.

### 6c — Data structure caps

Every `std::map` and `std::set` in Transport that accumulates entries over time needs:
1. A size cap appropriate for ESP32 memory constraints
2. An eviction strategy (timestamp-based, LRU, or lexicographic)
3. Culling in the `jobs()` periodic task

---

## 7. Packet Hashlist Timing Bug — Link Transport Breakage on Shared Media

### Bug 7a — Premature packet hash insertion breaks link transport (FIXED)

**File:** `Transport.cpp`, inbound() after packet_filter

**Bug:** The C++ code unconditionally added every accepted packet's hash to
`_packet_hashlist` immediately (line ~1505), before link transport or proof
handling ran:
```cpp
_packet_hashlist.insert(packet.packet_hash());  // Always, immediately
```

The Python reference implementation (`Transport.py` lines 1362-1373)
**defers** insertion for two cases:
1. Packets whose `destination_hash` is in `link_table` (link data packets)
2. LRPROOF packets (type=PROOF, context=LRPROOF)

For link data: the hash is added later, **inside** the link transport
forwarding block, only after a valid outbound direction is confirmed
(`Transport.py` line 1544).

For LRPROOF: the hash is **never** added (allowing duplicate proofs to be
processed on multiple interfaces).

**Impact:** On shared-medium interfaces (e.g. LoRa), a packet belonging to a
link that transports through this node may arrive on the "wrong" interface
first (e.g. received on LoRa before it arrives via TCP backbone). The
premature hash insertion causes the correct arrival to be filtered as a
duplicate for non-resource contexts (DATA ctx=0, LINKIDENTIFY, LRRTT,
LINKCLOSE). Resource contexts (RESOURCE, RESOURCE_REQ, RESOURCE_PRF)
are unaffected because they bypass the hashlist check in `packet_filter`.

**Fix:** Defer hash insertion for link-table and LRPROOF packets, matching
the Python reference implementation. Add `_packet_hashlist.insert()` inside
the link transport forwarding block after direction is confirmed.

---

## 8. Missing Link MTU Clamping — 70% Resource Transfer Stall (CRITICAL)

### Summary

**Status:** FIXED (v1.0.12, 2026-02-28)

`Transport.cpp` did not clamp the link MTU when forwarding `LINKREQUEST` packets through the transport node. The Python reference implementation (`Transport.py` lines 1458–1480) performs this clamping to ensure the negotiated link MTU does not exceed the capacity of any intermediate hop's interface.

### Bug 8a — LINKREQUEST forwarded without MTU clamping

**File:** `Transport.cpp`, all three LINKREQUEST forwarding paths:
1. Standard transport forwarding (next-hop routing, ~line 1729)
2. Boundary mode: local → backbone (~line 1875)
3. Boundary mode: backbone → local (~line 1959)

**Code (before fix):**
```cpp
if (packet.packet_type() == Type::Packet::LINKREQUEST) {
    // ... creates link_entry, inserts into _link_table ...
    // MTU signalling bytes in new_raw are forwarded UNCHANGED
}
```

The Python reference (`Transport.py` lines 1458–1480) does:
```python
path_mtu = RNS.Link.mtu_from_lr_packet(packet)
if path_mtu:
    nh_mtu = outbound_interface.HW_MTU
    ph_mtu = interface.HW_MTU if interface else None
    if nh_mtu < path_mtu or (ph_mtu and ph_mtu < path_mtu):
        path_mtu = min(nh_mtu, ph_mtu)
        clamped_mtu = RNS.Link.signalling_bytes(path_mtu, mode)
        new_raw = new_raw[:-RNS.Link.LINK_MTU_SIZE] + clamped_mtu
```

**Impact:** When both endpoints connect via TCP (HW_MTU=8192) through a V3 boundary node (HW_MTU=1064):

1. Sender's `LINKREQUEST` signals 8192-byte link MTU.
2. V3 forwards the request **unchanged** to the receiver.
3. Receiver confirms 8192-byte MTU → resource segments sized at ~7500 bytes (6 parts for a 46 KB file).
4. V3's HDLC deframer buffer (`rxbuf[1064]`) **silently truncates** oversized segments.
5. Only 4 of 6 truncated segments partially survive → receiver times out waiting for remaining parts → permanent stall at ~70%.

**Symptom:** LXMF resource transfers through the V3 boundary node stall permanently at ~70% progress. The sender keeps retrying but never completes.

### Bug 8b — `TcpInterface` does not declare `FIXED_MTU`

**File:** `TcpInterface.h`, constructor

**Code (before fix):**
```cpp
_HW_MTU = TCP_IF_HW_MTU;  // 1064
// _FIXED_MTU defaults to false
```

**Impact:** Even if Transport had MTU clamping code, it would skip clamping for this interface because `FIXED_MTU()` returns `false`. The interface's `HW_MTU` value is not treated as authoritative.

### Bug 8c — HDLC deframer silently truncates oversized frames

**File:** `TcpInterface.h`, `_hdlc_deframe()`

**Code (before fix):**
```cpp
if (c.rxlen < TCP_IF_HW_MTU) {
    c.rxbuf[c.rxlen++] = byte;
}
// Else: byte silently discarded, truncated frame delivered as if complete
```

**Impact:** When a client sends a frame larger than `TCP_IF_HW_MTU` (1064 bytes), the deframer silently drops bytes beyond the buffer limit and delivers the truncated frame to Transport as if it were complete. This corrupts resource data segments and hashmap updates, causing the resource transfer protocol to stall.

### Diagnosis

**Serial log evidence (pre-fix):**
- `LINK-XPORT: FWD` entries show 5 `RESOURCE_DAT` (ctx=1) segments forwarded, each silently truncated to 1064 bytes (from ~7500).
- After the 5th segment, no more `LINK-XPORT` entries — the receiver's `RESOURCE_HMU` (hashmap update, ctx=4) response is also truncated/corrupted and never processed.
- Receiver log: `"Timed out waiting for 4 parts, requesting retry"` — retry also stalls.

**Sender log evidence (pre-fix):**
```
Signalling link MTU of 8.19 KB for link
Destination confirmed link MTU of 8.19 KB    ← should have been clamped to 1064
The transfer of <LXMessage ...> is in progress (70.0%)   ← stuck forever
```

### Fix

#### 8a — MTU clamping in `Transport.cpp` (3 locations)

Added MTU clamping logic to all three `LINKREQUEST` forwarding paths. When the path MTU in the link request exceeds `min(prev-hop HW_MTU, next-hop HW_MTU)`, the signalling bytes are rewritten using `Link::signalling_bytes()`. If the outbound interface has no MTU or doesn't support MTU configuration, the signalling bytes are stripped entirely.

```cpp
uint16_t path_mtu = Link::mtu_from_lr_packet(packet);
if (path_mtu > 0) {
    uint16_t ph_mtu = packet.receiving_interface().HW_MTU();
    uint16_t nh_mtu = outbound_interface.HW_MTU();
    if (nh_mtu == 0) {
        new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
    } else if (!outbound_interface.AUTOCONFIGURE_MTU() && !outbound_interface.FIXED_MTU()) {
        new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
    } else if (nh_mtu < path_mtu || (ph_mtu > 0 && ph_mtu < path_mtu)) {
        uint16_t clamped = std::min(nh_mtu, (ph_mtu > 0) ? ph_mtu : nh_mtu);
        auto mode = Link::mode_from_lr_packet(packet);
        Bytes clamped_mtu_bytes = Link::signalling_bytes(clamped, mode);
        new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE) + clamped_mtu_bytes;
    }
}
```

#### 8b — `FIXED_MTU` in `TcpInterface.h`

Set `_FIXED_MTU = true` in the constructor so Transport uses the interface's `HW_MTU` (1064) for clamping decisions.

#### 8c — Truncation detection in `TcpInterface.h`

Added a `truncated` flag to `TcpClient`. Frames exceeding `TCP_IF_HW_MTU` are now **dropped with a diagnostic log** instead of silently truncated:

```
[TcpIF] DROPPED oversized frame from client 1 (>1064 bytes, buffered 1064)
```

### Verification

**Sender log (post-fix):**
```
Signalling link MTU of 8.19 KB for link
Destination confirmed link MTU of 1.06 KB    ← clamped!
*** DELIVERY RESULT: DELIVERED (state=8) elapsed=2.8s ***
```

**V3 serial log (post-fix):**
```
MTU CLAMP: path=8192 ph=1064 nh=1064 -> clamped=1064
```

**File integrity:** SHA-256 of received `test.pdf` matches original (`9bcb7b21d2bc7bbf...`).

### Test Reproduction

```bash
cd test-harnesses/RNodeTHV4
bash run_test.sh
```

Sends `test.pdf` (46.1 KB) as an LXMF attachment through the V3 boundary node. Pre-fix: stalls at 70%. Post-fix: delivers in ~3 seconds.

