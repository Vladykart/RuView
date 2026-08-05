# RuView HW/FW Task Board — StickS3 / CSI Mesh

> **System:** WiFi CSI sensing mesh (M5Stack StickS3 K150 nodes, ST7789 135x240 display, M5PM1 power gate) streaming CSI to the sensing server at 192.168.178.35:5005.
> **Firmware:** `firmware/esp32-csi-node/` (ESP-IDF, C). Rust side: `rust-port/wifi-densepose-rs/crates/wifi-densepose-hardware/src/esp32/` and `wifi-densepose-sensing-server`.
> **Branch:** `sticks3-display` on fork Vladykart/RuView, HEAD 9de7fd89, 5 commits ahead of ruvnet `main` (8a84748a). No PR open yet.
> **Node 1:** deployed at 192.168.178.41 — display working, OTA on :8032, streaming, edge_tier 2. OTA auth currently DISABLED; no BSSID filter provisioned; no rollback protection.
> **Owners:** `pop-os` = local machine, hardware/serial/network access to node 1. `VPS` = srv1834218 agent, code/review only (SSH currently down, see Blocked).

---

## 1. Done this session (2026-08-05)

- **Node 1 StickS3 deployed and verified on real hardware** — ST7789 status UI running at 192.168.178.41, OTA :8032, CSI streaming to 192.168.178.35:5005 at edge_tier 2 (commits e8758176, d5a82c27; `firmware/esp32-csi-node/main/display_hal.c`, `display_ui.c`).
- **Board identity review blocker CLOSED** — confirmed M5Stack StickS3 K150 (ST7789 135x240 + M5PM1 power gate) on physical hardware.
- **OTA max-size cap fix landed** — `OTA_MAX_SIZE` = 0x1D0000 (1.875 MB) made node-1 OTA work (`firmware/esp32-csi-node/main/ota_update.c:26-27`; comment still stale, see Later).
- **ADR-065 TDMA round-robin CSI mesh written and committed** (9de7fd89, `docs/adr/ADR-065-tdma-round-robin-csi-mesh.md`, status Proposed, 5 open review questions).
- **Mock-CSI verified compiled out of release builds** — `main/CMakeLists.txt:11` gates `mock_csi.c` on `CONFIG_CSI_MOCK_ENABLED`, `Kconfig.projbuild:220-222` defaults `n`, no `sdkconfig.defaults*` enables it. No task needed.
- **No-credentials policy verified currently met** — no SSID/password overrides in any `sdkconfig.defaults*`; placeholders only in `Kconfig.projbuild`. (CI enforcement still to add, see Next.)
- **TDM foundations verified in place** — `tdm_slot_index`/`tdm_node_count` in `nvs_config.h:36-37`, `nvs_config.c:182-196,305-309`; `provision.py --tdm-slot/--tdm-total`; Rust `tdm.rs`/`secure_tdm.rs`/`quic_transport.rs`.

---

## 2. Now (unblocked, ordered)

### 2.1 Revert clobbered `sdkconfig.defaults` (C6 overwrite) — owner: pop-os
`firmware/esp32-csi-node/sdkconfig.defaults` (tracked) is overwritten in the working tree with ESP32-C6 config, byte-identical to the already-tracked `sdkconfig.defaults.esp32c6`. Committing it flips the default build target from S3 to C6 and breaks StickS3 builds. **Do this before any commit on the branch.**
- **Fix:** `git checkout -- firmware/esp32-csi-node/sdkconfig.defaults`
- **Accept:** `git diff` shows `sdkconfig.defaults` back to `CONFIG_IDF_TARGET=esp32s3` / `partitions_display.csv`; C6 content preserved only in `sdkconfig.defaults.esp32c6`.

### 2.2 Commit the uncommitted StickS3 firmware fixes — owner: pop-os
Commit exactly these two files (after 2.1, never `git add -A`):
- `firmware/esp32-csi-node/main/edge_processing.c` — tick-starvation fix (`vTaskDelay(1)` minimum; `pdMS_TO_TICKS(1)==0` at 100 Hz) + dsp_priority 1→2 (above display, below WiFi).
- `firmware/esp32-csi-node/main/main.c` — WPA2/WPA3-transition join robustness (`pmf_cfg.capable`, `WPA3_SAE_PWE_BOTH`, `WIFI_ALL_CHANNEL_SCAN`) for the deployed FRITZ!Box network.
- **Accept:** commit on `sticks3-display` contains only these two files; pushed to fork.

### 2.3 Rebuild StickS3 firmware and OTA-verify node 1 — owner: pop-os
The scheduling change (2.2) alters display/DSP behavior on the deployed node; CLAUDE.md release process step 7 requires real-hardware, real-CSI verification.
- **Method:** build from `firmware/esp32-csi-node/sdkconfig.defaults.sticks3` (cp to `sdkconfig.defaults`, `idf.py set-target esp32s3 build`; keeps `partitions_4mb.csv` dual 1.875 MB OTA slots), OTA to 192.168.178.41:8032.
- **Accept:** ST7789 display updates normally, CSI streams to 192.168.178.35:5005 at edge_tier 2, no watchdog/starvation symptoms.

### 2.4 Add `--ota-psk` to provision.py and enable OTA auth on node 1 — owner: pop-os (VPS may draft code)
OTA PSK read from NVS namespace `security` key `ota_psk` (`main/ota_update.c:30-31,250-256`; empty PSK = auth disabled, `ota_check_auth` :46-48). `provision.py` only writes the `csi_cfg` namespace (`build_nvs_csv` :33-38; argparse :173-197 has no flag). **Node 1 currently runs OTA with auth disabled.**
- **Accept:** `--ota-psk` emits a `security` namespace + `ota_psk` string row in the NVS CSV; node 1 re-provisioned; OTA request without PSK rejected, with PSK accepted.

### 2.5 Resolve wasm_verify boot warning: `--wasm-pubkey` / `--wasm-verify` flags — owner: pop-os (VPS may draft code)
Boot warns "wasm_verify=1 but no wasm_pubkey in NVS — uploads will be rejected" (`main/nvs_config.c:282`; default-on at :89; 32-byte blob key `wasm_pubkey` in `csi_cfg`, :271-280; `wasm_upload.c:116-129` rejects all uploads without it). `provision.py` cannot write either value.
- **Accept:** `--wasm-pubkey <hex32>` (hex2bin blob row) and optional `--wasm-verify 0|1` in provision.py; node 1 provisioned with its Ed25519 pubkey; boot warning gone; signed upload accepted.

### 2.6 Provision BSSID/MAC source filter on node 1 — owner: pop-os
Firmware support exists (ADR-060): NVS MAC filter drops non-matching CSI frames (`main/csi_collector.c:159-163`), provisioned via `provision.py --filter-mac` (:196). Operational only — fold into the 2.4/2.5 re-provisioning pass.
- **Accept:** node 1 provisioned with the AP BSSID; frames from neighboring transmitters no longer ingested; `--filter-mac` documented as standard procedure for nodes 2/3.

### 2.7 Implement OTA rollback + capture on-hardware rollback evidence — owner: pop-os
No call to `esp_ota_mark_app_valid_cancel_rollback` anywhere in `main/` (`ota_update.c` only calls `esp_ota_set_boot_partition` at :180); `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in zero `sdkconfig.defaults*` files. Independent-review blocker (rollback evidence) still open.
- **Accept:** rollback enabled in `sdkconfig.defaults(.sticks3)`; mark-valid called after post-boot health check (WiFi join + CSI streaming); real rollback test recorded on node 1 (flash deliberately failing image via :8032, bootloader reverts, evidence logged).

### 2.8 ADR-065 evidence: measure esp_timer drift + TX-scheduling jitter under WiFi load (review Q2) — owner: pop-os
Node-1-only, no mesh code. Timestamp `esp_timer_get_time()` against server clock via UDP round-trips over 30+ min while CSI streaming is active; measure esp_timer callback jitter (reuse hop timer machinery, `main/csi_collector.c:357-398`) and `esp_wifi_set_channel`/promiscuous-enable latency under load.
- **Accept:** measured drift/jitter numbers compared against the ±10 ppm assumption in `esp32/tdm.rs` (header comment) and ADR-065 `resync_interval_ms=5000` / `max_clock_error_us=300`; logged for the ADR update (3.1).

### 2.9 ADR-065 evidence: validate `esp_wifi_80211_tx` while STA-associated — owner: pop-os
The NDP stub (`main/csi_collector.c:402-438`) has never run on hardware. Needs one TX-capable bench device.
- **Accept:** confirmed whether `esp_wifi_80211_tx` accepts the frame while associated; measured TX rate/airtime; determined whether hardware overwrites Addr2 with own MAC and whether AP/IDF blocks broadcast-BSSID null-data. Result feeds ADR-065 Q1 transport decision.

### 2.10 ADR-065 evidence: peer→peer promiscuous CSI capture at driver level (review Q1) — owner: pop-os
Prove RX node's `wifi_csi_callback` (`main/csi_collector.c:155-203`) fires on the true peer PHY preamble, not AP relay: bench TX device calls `csi_inject_ndp_frame` (:402-438) while node 1 captures with the ADR-060 MAC filter (:159-164) set to the TX MAC; compare CSI count/RSSI vs AP-sourced frames. Compare the three ADR-065 Q1 candidate transports: raw 802.11 injection vs UDP-via-AP vs ESP-NOW-style peer frames. Promiscuous filter already passes MGMT|DATA (:247-250). *Needs a bench TX device — full comparison is cleanest with node 2, see 3.4.*
- **Accept:** per-transport CSI capture counts/RSSI table recorded for the ADR-065 Q1 answer.

---

## 3. Next (after Now, or after hardware arrives)

### 3.1 Update ADR-065 with measurement evidence, drive review closure — owner: VPS (doc), inputs from 2.8-2.10
Fold Q1/Q2 evidence into `docs/adr/ADR-065-tdma-round-robin-csi-mesh.md` "Open questions from review"; propose answers for Q3 (ADR-018 contract versioning plan) and Q4 (coordinator failure = peers stop TX on sync loss, criterion 9); move toward Accepted. **Gates the entire Blocked mesh-implementation set.**

### 3.2 Add version field to the CSI record contract — owner: VPS
Raw ADR-018 header (`main/csi_collector.c:73-87`, magic 0xC5110001) has reserved bytes [18..19] but no version field; edge packets (`edge_processing.h` magics 0xC5110002/3/4) are magic-only. Use a reserved byte as format version; update the sensing-server parser (`rust-port/.../wifi-densepose-sensing-server`) in lockstep. Needed before nodes 2/3 join so mixed-firmware meshes stay parseable.

### 3.3 Record display visual acceptance evidence for the StickS3 UI — owner: pop-os
Last open independent-review blocker besides rollback. Capture photos of each UI card/state on the physical device (boot, WiFi connected, CSI streaming, OTA in progress) for `main/display_ui.c` / `display_hal.c` StickS3 path (:221-365, commits e8758176/d5a82c27); attach to the review record on the VPS or the PR.

### 3.4 Flash and provision second StickS3 as node 2 — owner: pop-os *(when second StickS3 is in hand)*
Not blocked by ADR-065 (FIXED_ANCHOR-style AP-illuminator mode remains the default). Build from `sdkconfig.defaults.sticks3`; flash over USB respecting the USB-JTAG download-latch rule (physical RST, never soft-reset over native USB); provision via `provision.py` with `--node-id 2`, distinct `--tdm-slot` (`--tdm-slot 1 --tdm-total 2`, provision.py:50-53), `--filter-mac` (AP BSSID), `--target-ip 192.168.178.35`, `--edge-tier 2`, plus `--ota-psk`/`--wasm-pubkey` from 2.4/2.5. Set room position server-side (per-node positions, commit 1a41a2a1). Verify both nodes stream concurrently to :5005 with distinct node IDs (`csi_collector.c:121`). Prerequisite for full ADR-065 peer-capture and multistatic testing.

### 3.5 Decide fate of provision.py chip auto-detect diff — owner: pop-os
Working-tree diff replaces hardcoded `--chip esp32s3` with `detect_chip()` via `esptool chip_id` (`firmware/esp32-csi-node/provision.py`). Needed for node 2/3 anyway, but clean up first: the `.upper().replace` chain is a no-op transform and `import re` sits inside the function. Commit on `sticks3-display` or move to the C6 branch.

### 3.6 Isolate non-firmware working-tree changes off `sticks3-display` — owner: pop-os
Keep out of firmware commits: `wifi-densepose-sensing-server/src/vital_signs.rs` (+135), `wifi-densepose-cli/src/mat.rs`, `wifi-densepose-desktop/ui/package-lock.json`, `wifi-densepose-desktop/gen/schemas/linux-schema.json`, `ui/*`, and root-level `occupancy_trigger.py` + `test_ruview.py` (CLAUDE.md forbids root working files — relocate to `scripts/`/`v1/tests/` or delete). Stash or branch separately; never `git add -A` here.

### 3.7 Decide `sdkconfig.defaults.esp32` (original ESP32) — delete or justify — owner: pop-os
Untracked `firmware/esp32-csi-node/sdkconfig.defaults.esp32` targets original ESP32, which CLAUDE.md lists as NOT supported (single-core, can't run CSI DSP). Delete, or keep on an experiment branch with an explicit caveat; don't commit here without updating the supported-hardware table.

### 3.8 Add no-credentials-in-image CI guard — owner: VPS
Policy currently met (verified) but unenforced. Add a `.github/workflows/firmware-ci.yml` step failing if the built `esp32-csi-node.bin` (artifact paths :95-99) or `sdkconfig` contains a non-placeholder SSID/password.

### 3.9 StickS3 backlight: LEDC PWM brightness — owner: pop-os (verify on hardware)
StickS3 path drives backlight GPIO38 as on/off GPIO (`main/display_hal.c:240,307-313`; `display_hal_set_brightness` :387-391 is binary). M5GFX uses LEDC PWM ch7 @256Hz on this pin; the sibling ST7789 variant in the same file already has LEDC (:109-128, :213-218). Port it and honor `CONFIG_DISPLAY_BRIGHTNESS` (`Kconfig.projbuild:120-124`), currently ignored by the StickS3 path.

### 3.10 Pre-merge docs pass: CHANGELOG, README, CLAUDE.md, ADR index — owner: VPS
All missing per Pre-Merge Checklist 3-7: `CHANGELOG.md` [Unreleased] (:31) lacks StickS3 K150 / ST7789 / OTA-httpd-fix entry; `README.md` has no StickS3 K150 row (hardware tables ~:62, ~:763) and says "62 ADRs" at :78 while `docs/adr/` has 65; CLAUDE.md hardware table lacks StickS3 and says "43 ADRs"; `docs/adr/README.md` index stops at ADR-044 — at minimum add ADR-065 (ADR-045..064 gap is pre-existing on main; see Later).

### 3.11 Open PR Vladykart:sticks3-display → ruvnet/RuView:main — owner: VPS (pop-os pushes final commits first)
No PR exists. Branch includes fe58b839 (ESP32-C6 display + multi-node fixes) which rides along — PR description must cover both C6 and StickS3 scope, or split fe58b839 first. Check overlap with upstream draft PR #1529 (SaschaMuehl, same firmware area). Do after 3.10 and hardware verification (2.3). PR body ends with claude-flow attribution.

### 3.12 ESP32-S3-BOX-3 display variant (ILI9342C 320x240) — owner: pop-os *(when BOX-3 is in hand; code prep VPS-draftable)*
`Kconfig.projbuild` has only AMOLED/RM67162 (menu :91, `DISPLAY_ENABLE` :93) and `DISPLAY_STICKS3` (:101-106). Add a `DISPLAY_BOX3` choice, third init path in `main/display_hal.c` (StickS3 block ~:227-391, AMOLED from :393) using esp_lcd ILI9342C, and `sdkconfig.defaults.box3` alongside `.sticks3`/`.esp32`. Pure board bring-up is independent of ADR-065; coordinator role is Blocked (4.10).

---

## 4. Blocked

### 4.1 Re-authenticate Tailscale SSH to srv1834218; re-read both TWPS reports — owner: user + VPS
**Blocker: Tailscale SSH interactive browser check (user must visit the printed login.tailscale.com URL); direct port 22 firewalled on all routes.** Host alive (tailscale ping 21ms). Then re-read `/root/kun-agent-workspace/data/twps-csi-firmware-upstream-s2/report.md` and `/root/kun-agent-workspace/data/twps-sticks3-csi-independent-review-s1/report.md` to confirm the constraint tasks here match the reports' exact wording.

### 4.2 Verify `STATUS-sticks3-2026-08-05.md` landed on the VPS; re-send if missing — owner: VPS
**Blocker: same SSH outage (4.1).** Check `/root/kun-agent-workspace/projects/ruview/STATUS-sticks3-2026-08-05.md`; if missing, re-send the node 1 status report.

### 4.3 Define `csi_mesh_probe_t` probe header (firmware struct + Rust mirror) — owner: VPS (code) after unblock
**Blocker: ADR-065 review Q1 (probe transport); ADR is Proposed — no firmware implementation until review closes.** Per ADR-065 "Collision protection": magic, protocol+schedule versions, session_id, superframe_id, transmitter_node_id, slot_id, packet_in_slot, global_sequence, scheduled/actual TX time, config_hash, crc32. New `firmware/esp32-csi-node/main/csi_mesh.h` + mirrored parser in `esp32/` Rust with round-trip tests (pattern: `SyncBeacon::to_bytes/from_bytes`, `tdm.rs:333-346`).

### 4.4 NVS + provisioning plumbing for mesh_csi config and the three profiles — owner: VPS/pop-os
**Blocker: ADR-065 Proposed; field set (tx_order/session semantics) depends on review.** Extend `nvs_config_t` (`main/nvs_config.h`) with profile enum (FIXED_ANCHOR|ROUND_ROBIN|PAIR_SCAN), mesh enabled, node_count, tx_order[], superframe_us, slot_us, guard_us, packets_per_slot, resync_interval_ms, max_clock_error_us, collision_limit; load/validate per `nvs_config.c:182-196,305-309` pattern; provision.py flags; runtime setter on :8032 (pattern: `wasm_upload.c`/`ota_update.c`) for no-reflash switching (criterion 11). Default FIXED_ANCHOR preserves deployed behavior.

### 4.5 Slot scheduler state machine in firmware (CSI_PEER role) — owner: pop-os
**Blocker: ADR-065 Q2 (slot timing evidence) and Q1 (transport).** IDLE → SESSION_PREPARE → TIME_SYNC → WAIT_EPOCH → SUPERFRAME_LOOP with TX_BURST/RX_LISTEN + RECOVERY/RESYNC; `slot_index = ((local_time_us - epoch_start_us)/slot_us) % node_count`. New module in `main/`; reuse esp_timer patterns (`csi_collector.c:357-398`). All three profiles; strict single-radio TX/RX switching.

### 4.6 Time sync client: clock_offset_us/drift_ppm + schedule.prepare handling — owner: pop-os
**Blocker: ADR-065 Q2 (clock tolerance) and Q4 (coordinator failure modes).** Maintain clock_offset_us, clock_drift_ppm, last_sync_timestamp, schedule_version; smooth corrections (never jump slots mid-superframe); handle schedule.prepare JSON. Criterion 9: sync loss ⇒ stop transmitting. Coordinate with Rust SyncBeacon/drift model (`tdm.rs:317-346`, `TdmCoordinator` :396+).

### 4.7 Collision detection, superframe invalidation, RESYNC — owner: pop-os
**Blocker: depends on 4.3 + 4.5; ADR-065 Proposed.** RX-side probe validation (transmitter-vs-slot, schedule_version, session_id, allowlist); dual-TX ⇒ superframe invalid; after collision_limit (3) stop recording, RESYNC with new epoch (criteria 8-9). Counters surfaced on `display_ui.c` and /status.

### 4.8 ADR-018 packet contract v2: mesh CSI record fields, versioned atomically — owner: VPS
**Blocker: ADR-065 Q3 (ADR-018/TWPS contract versioning); all consumers version together.** Extend the 20-byte header in `csi_serialize_frame` (`main/csi_collector.c:73-150`, magic 0xC5110001) with superframe_id, slot_id, transmitter/receiver_node_id, packet_in_slot, directed_link_id, schedule_version, expected_tx/actual_rx times — new versioned magic, never silent. Firmware serializer + sensing-server parser + TWPS 14-field CSV + fixtures in one change. Also rework flat 50 Hz UDP limiter (`CSI_MIN_SEND_INTERVAL_US`, `csi_collector.c:52,181-195`) to per-directed-link (as-is drops most of an 8-packet slot burst, ~266 pkt/s).

### 4.9 Extend Rust TDM protocol for ADR-065 profiles; reconcile timing constants — owner: VPS
**Blocker: ADR-065 Proposed; probe header + timing profile must settle first.** `tdm.rs` models 4ms slot/1ms guard/50ms cycle (DEFAULT_SLOT_MS=4, DEFAULT_GUARD_US=1000, default_4node) vs ADR-065's 20ms/2ms/60ms 3-node superframe. Add profiles, 60ms 3-node schedule, schedule_version/session_id/config_hash, probe parsing; keep `TdmSchedule::uniform`; consider `secure_tdm.rs` (ADR-032) for the allowlist.

### 4.10 ESP32-S3-BOX-3 as node 3 + coordinator role — owner: pop-os
**Blockers: (a) BOX-3 hardware not yet deployed (bring-up = 3.12); (b) ADR-065 Q4 (coordinator failure modes) — ADR Status explicitly forbids firmware implementation/hardware action until review closes.** Coordinator: schedule distribution, epoch start, readiness checks, dead-node detection, new schedule_version at superframe boundaries only, management traffic in management slot, safe degradation (peers stop TX on sync loss).

### 4.11 Mesh acceptance validation: 12 ADR-065 criteria + 30-minute soak — owner: pop-os
**Blocker: requires all three nodes deployed and 4.3-4.10 complete.** No dual-TX per slot, slot skew in limit, TX→RX→RX cycling, superframe_id/slot_id on every frame, server distinguishes 6 directed links, node-loss reschedule, boundary-only schedule changes, auto collision detection, desync ⇒ TX stop, 30 min no drift-induced overlap, no-reflash profile switch, fleet-wide schedule_version/config_hash agreement. Log evidence into the ADR; positioning/breathing claims out of scope (Q5).

---

## 5. Later / optional

- **Real 802.11 NDP frame construction (ADR-029 TX path)** — `csi_inject_ndp_frame` (`main/csi_collector.c:402-438`, TODO at :405; also `csi_collector.h:80`) is a minimal 24-byte null-data stand-in; proper preamble-only NDP (~24us airtime) needed. Frame construction itself is independent of the ADR-065 review. Owner: pop-os to verify.
- **Fix stale `OTA_MAX_SIZE` doc comment** — `main/ota_update.c:26-27` says "900 KB — matches CI binary size gate" but value is 0x1D0000 = 1.875 MB (partitions_4mb.csv). Owner: VPS.
- **Optional: skip/shorten mmWave UART probe when no sensor fitted** — `main/mmwave_sensor.c` probes MR60BHA2 then LD2410 (`probe_sensor` :416, `probe_at_baud` :373, 2000 ms timeout :51); StickS3 nodes pay it for nothing. Kconfig/NVS switch. Probing is by design (ADR-063), harmless. Owner: VPS.
- **Decide whether firmware CI builds the StickS3 display variant** — `.github/workflows/firmware-ci.yml` builds only base esp32s3 (:27); deployed `sdkconfig.defaults.sticks3` (DISPLAY_STICKS3, `Kconfig.projbuild:101`) never compiles in CI, so `display_hal.c`/`display_ui.c` regressions escape. Add `SDKCONFIG_DEFAULTS=sdkconfig.defaults.sticks3` step or document the exclusion. Verify against upstream report wording once SSH restored (4.1). Owner: VPS.
- **Regenerate witness bundle at merge time** — branch changed `display_ui.c`, `ota_update.c`, `edge_processing.c`, `main.c`, `sdkconfig.defaults.sticks3` covered by `firmware-manifest/source-hashes.txt`. Run `cargo test --workspace --no-default-features` (1,031+ pass) + `python v1/data/proof/verify.py` (PASS) then `bash scripts/generate-witness-bundle.sh`, VERIFY.sh 7/7. Owner: pop-os.
- **Commit `.gitignore` change; fix tracked-but-ignored `sdkconfig.defaults`** — diff adds `.gstack/` (safe). `.gitignore:6-7` ignores `firmware/esp32-csi-node/sdkconfig.defaults` while git tracks it — inert rule that let the C6 clobber nearly slip in. Either `git rm --cached` and rely on per-target files (`.sticks3`/`.esp32c6`/`.4mb`/`.template`), or drop the stale ignore lines. Owner: pop-os.
- **ADR index backfill ADR-045..064** — `docs/adr/README.md` stops at ADR-044; gap is pre-existing on main, separate cleanup from the 3.10 docs pass. Owner: VPS.
