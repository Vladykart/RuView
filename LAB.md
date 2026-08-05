# RuView Lab — team & processes

The lab runs as a team of specialist agents (definitions in `.claude/agents/lab-*.md`)
over a live 3-node WiFi-CSI installation plus a camera-truth pipeline.

## Team
| Agent | Owns |
|---|---|
| lab-data-engineer | collectors, dataset formats, golden runs, manifests |
| lab-data-scientist | experiments, metrics, research reports, spectra |
| lab-radio-engineer | RF physics, ADR-065 mesh, spectral interpretation |
| lab-hardware-specialist | nodes, sensors, wiring diagnosis, OTA ops |
| lab-mlops | training runs, VPS offload, reproducibility |
| lab-firmware-engineer | firmware dev, OTA-shippable queue, releases |
| lab-ops | herdr cockpit, services, ruview-up, dashboards |

## Standing processes
1. **Golden dataset run**: data-engineer arms collectors (+manifest) → scientist
   schedules research passes → finalize consolidates, commits locally, ships to
   VPS via Taildrop. Never OTA/reboot nodes mid-run.
2. **Firmware ship**: firmware-engineer builds per-board profiles → hardware-
   specialist OTAs fleet outside collection windows → verify /ota/status +
   streams → commit with hardware evidence.
3. **Morning report**: research reports + MEETING.md in the dataset dir; honest
   caveats mandatory (calibration state, room geometry, subject conditions).
4. **Recovery**: `ruview-up` after any restart; NIGHT tab shows the machine.

## Ground rules
- Provenance discipline everywhere (measured/fused/simulated/replay).
- Private by default: datasets local, no remotes for data repos.
- Hardware truth beats assumptions: verify on the bench, `/ota/diag` first.
- Honest metrics: segment by condition; never present flattered numbers.
