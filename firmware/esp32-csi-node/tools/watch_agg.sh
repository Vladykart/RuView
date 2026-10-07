#!/usr/bin/env bash
# Live aggregator (RuView sensing-server) feed
URL="${TWPS_AGG:-http://127.0.0.1:8080}/api/v1/sensing/latest"
while true; do
  clear
  echo "== Aggregator live ==  $(date '+%F %H:%M:%S')   ($URL)"
  curl -s -m3 "$URL" | python3 -c '
import sys,json
try: d=json.load(sys.stdin)
except Exception: print("  no data / unreachable"); sys.exit()
c=d.get("classification",{}); f=d.get("features",{})
print("  presence :", c.get("presence"), " persons:", d.get("estimated_persons"), " motion:", c.get("motion_level"))
print("  mean_rssi:", f.get("mean_rssi"), " breathing_bp:", round(f.get("breathing_band_power",0) or 0,1), " motion_bp:", round(f.get("motion_band_power",0) or 0,1))
ns=d.get("nodes") or []
print("  streaming node_ids:", sorted(set(n.get("node_id") for n in ns)))
for n in ns: print("    node %s  rssi %s dBm  %s subcarriers" % (n.get("node_id"), n.get("rssi_dbm"), n.get("subcarrier_count")))
vs=d.get("vital_signs") or {}
if vs: print("  vitals:", {k:vs.get(k) for k in ("heart_rate_bpm","breathing_rate_bpm") if k in vs})
'
  sleep 2
done
