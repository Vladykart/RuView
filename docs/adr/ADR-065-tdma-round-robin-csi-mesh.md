# ADR-065: TDMA Round-Robin CSI Mesh (3× CSI_PEER)

## Status

**Proposed — under independent review.** An independent reviewer is validating
this design against pinned ESP-IDF, TWPS, and RuView behavior before any
hardware implementation. See "Open questions from review" below. No firmware
implementation or hardware action until those close.

## Context

The current deployment uses fixed roles: one node transmits (or sniffs an AP
illuminator) and others receive. With three identical ESP32-S3 class nodes
available (StickS3 node 1, a second StickS3, and an ESP32-S3-BOX-3), a
time-division round-robin architecture is stronger: every node takes a turn
transmitting while the others capture CSI.

One full cycle yields **six directed measurements**:

```
Slot 1: A TX  ->  B, C RX     links A->B, A->C
Slot 2: B TX  ->  A, C RX     links B->A, B->C
Slot 3: C TX  ->  A, B RX     links C->A, C->B
```

Directions are stored separately (A->B ≠ B->A): the physical path is
reciprocal but each node's RF chain, LO phase offsets, gain, and noise are
not.

Compared to a fixed transmitter (2 links), the mesh gives 6 directed links,
overlapping sensitivity zones, no "passive-only" node, resilience to one
blocked link or one dead node, and a natural basis for between-node
localization. Constraint: the ESP32 has a single WiFi radio and cannot
usefully TX probes and capture CSI simultaneously — switching must be
strictly slot-based.

## Decision

### Roles

Replace fixed `TX_ANCHOR` / `RX_CSI` roles with a single universal role
**`CSI_PEER`**. All three boards run the same firmware; only `device_id`,
`node_id`, MAC, and placement differ. Per-slot behavior is derived from a
distributed schedule.

Node states: `IDLE -> SESSION_PREPARE -> TIME_SYNC -> WAIT_EPOCH ->
SUPERFRAME_LOOP`, with `TX_BURST` / `RX_LISTEN` inside the loop and
`RECOVERY`/`RESYNC` on faults.

### Network profiles (runtime-switchable, no reflash)

The firmware MUST support three profiles selectable by configuration only:

| Profile | Behavior | Use for |
|---|---|---|
| `FIXED_ANCHOR` | one node always TX, others always RX (the classic fixed-TX-node mode) | breathing / long continuous single-link series, max-rate tests, reference comparisons, diagnostics |
| `ROUND_ROBIN` | TDMA rotation per this ADR | positioning, room mapping, presence, movement, spatial dataset collection |
| `PAIR_SCAN` | one pair active for a long window (e.g. 5 s), then next pair | calibration, per-link quality diagnostics, placement tuning |

`FIXED_ANCHOR` is a first-class supported mode, not a fallback: breathing
models prefer an unbroken single-link time series, so the same fleet must be
able to drop back to a classic fixed transmitter at any time via config.

### Superframe schedule

Initial safe profile (3 nodes + management slot):

```
Slot 0: Node A TX   0-18 ms   | guard 18-20 ms
Slot 1: Node B TX  20-38 ms   | guard 38-40 ms
Slot 2: Node C TX  40-58 ms   | guard 58-60 ms
Slot 3: Management / sync / SILENCE (not necessarily every superframe)
```

- `slot_us = 20000`, active TX window 18 ms, `guard_us = 2000` (do not go
  below 1-2 ms initially), superframe 60 ms (~16.67 Hz) or 80 ms with the
  management slot.
- TX burst: 5-10 probe packets per slot (start `packets_per_slot: 8`), i.e.
  ~133 packets/s per directed link — sufficient for breathing as a base
  observation rate. A faster profile (10 ms slots, 1 ms guard, 30 ms
  superframe) only after switching stability and packet loss are proven.
- The management slot carries sync/heartbeat/commands/labels; for clean
  training recordings it can be a SILENCE slot. Running it once per 50-100
  superframes (not every cycle) preserves spatial frame rate.

### Synchronization

No token passing (accumulates jitter). A coordinator distributes a schedule
against shared time:

```json
{
  "command": "schedule.prepare",
  "session_id": 14028,
  "schedule_version": 3,
  "epoch_start_us": 823600000,
  "superframe_us": 60000,
  "slot_us": 20000,
  "guard_us": 2000,
  "tx_order": [0, 1, 2]
}
```

`slot_index = ((local_time_us - epoch_start_us) / slot_us) % node_count`.

Local `esp_timer_get_time()` alone is insufficient — each node maintains
`clock_offset_us`, `clock_drift_ppm`, `last_sync_timestamp`,
`schedule_version`, with initial sync plus periodic correction applied
**smoothly** (never jump slots inside an active superframe).

### Collision protection

Every probe packet carries a `csi_mesh_probe_t` header: magic, protocol +
schedule versions, session_id, superframe_id, transmitter_node_id, slot_id,
packet_in_slot, global_sequence, scheduled/actual TX time, config_hash,
crc32. Receivers validate transmitter-vs-slot, schedule_version, session_id,
and an allowlist. Two different transmitters seen in one slot =>
`collision_detected`, superframe marked invalid; after `collision_limit`
(default 3) the node stops recording and enters RESYNC with a new epoch.
A node that loses sync MUST stop transmitting rather than run on stale time.

### CSI record format (mesh extension)

Mesh CSI records add: `superframe_id`, `slot_id`, `transmitter_node_id`,
`receiver_node_id`, `packet_in_slot`, `directed_link_id`
(`A_TO_B` ... `C_TO_B`), `schedule_version`, `expected_tx_time_us`,
`actual_rx_time_us`. Server groups `session -> superframe -> directed_link`,
feeding models as `[time, directed_link, subcarrier, I/Q]`. This is a
versioned change to the ADR-018 packet contract — parser and fixtures must
update atomically.

### Coordinator (ESP32-S3-BOX-3)

The S3-BOX is simultaneously a full CSI peer and the logical coordinator:
schedule distribution, epoch start, readiness checks, health metrics,
labels, status display, dead-node detection, resync initiation. Management
traffic is minimized during CSI superframes (management window or between
sessions). On node loss the coordinator issues a new `schedule_version` with
the surviving `tx_order`; schedule changes take effect **only at a
superframe boundary**, never mid-slot.

### Mesh configuration

```json
{
  "role": "csi_peer",
  "node_id": 1,
  "mesh_csi": {
    "enabled": true,
    "scheduler": "tdma",
    "profile": "round_robin",        // round_robin | fixed_anchor | pair_scan
    "node_count": 3,
    "tx_order": [0, 1, 2],
    "superframe_us": 60000,
    "slot_us": 20000,
    "guard_us": 2000,
    "packets_per_slot": 8,
    "resync_interval_ms": 5000,
    "max_clock_error_us": 300,
    "collision_limit": 3
  }
}
```

## Acceptance criteria (ROUND_ROBIN)

1. No two nodes transmit in the same normal slot.
2. Slot-start skew between devices stays within the configured limit.
3. Each node cycles TX -> RX -> RX correctly.
4. Every CSI frame carries `superframe_id` and `slot_id`.
5. The server distinguishes all six directed links.
6. Node loss produces a new schedule.
7. Schedule changes occur only at superframe boundaries.
8. Collisions are detected automatically.
9. A desynchronized node stops transmitting.
10. 30 minutes of operation without drift-induced slot overlap.
11. `FIXED_ANCHOR`, `ROUND_ROBIN`, `PAIR_SCAN` switch by config, no reflash.
12. All nodes agree on `schedule_version` and `config_hash`.

## Open questions from review (blocking implementation)

1. **Direct-link validity.** On infrastructure WiFi, traffic may be relayed
   through the AP; it must be proven at driver level that each RX node
   captures CSI of the true peer->peer channel (promiscuous capture of the
   transmitter's PHY preamble) rather than AP->node. The transport for probe
   bursts (raw 802.11 vs UDP via AP vs ESP-NOW-style peer frames) must be
   chosen with evidence on pinned ESP-IDF.
2. **Timing claims.** 10-20 ms slots and ±300 µs clock tolerance need
   driver-level measurements (TX scheduling jitter, promiscuous-RX enable
   latency, esp_timer drift under WiFi load).
3. **Protocol impact.** Changes to the ADR-018 20-byte contract / TWPS
   14-field CSV are breaking; both consumers must be versioned together.
4. **Coordinator failure modes.** S3-BOX loss must degrade to a safe state
   (peers stop TX on sync loss — criterion 9).
5. **Claims discipline.** Positioning/breathing/production performance
   claims stay out of scope until measured.

## Relationship to existing work

- ADR-018 (UDP CSI packet), ADR-029 (channel hopping + TDM slots — NVS
  fields `tdm_slot_index` / `tdm_node_count` already exist in
  `nvs_config.h`), ADR-032 (mesh security hardening) and the existing
  `wifi-densepose-hardware` TDM protocol are the foundation; this ADR
  extends TDM from "who listens when" to "who transmits when".
- Existing deployment (AP-illuminator sniffing, `FIXED_ANCHOR`-like) remains
  the default until this ADR is Accepted.
