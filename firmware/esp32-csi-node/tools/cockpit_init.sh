#!/usr/bin/env bash
# cockpit_init.sh — one-command live cockpit for the TWPS 6-node WiFi-CSI mesh.
#
#   ┌───────────┬───────────┬────────────┐
#   │  health   │  battery  │ aggregator │  row1  (all-6 status / batt bars / feed+vitals)
#   ├───────────┼───────────┼────────────┤
#   │  rssi     │  csi-amp  │ services   │  row2  (per-node rssi/rate / CSI sparkline / up-down+URLs)
#   ├───────────┼───────────┼────────────┤
#   │  sysmon   │  netmon   │  clock     │  row3  (host + aesthetic)
#   └───────────┴───────────┴────────────┘
#
# Idempotent: tears down & rebuilds ONLY the MESH-COCKPIT tab, leaving all other
# tabs (incl. the generic dev COCKPIT + its agents) untouched. Panes read live
# data over HTTP — never the serial port.
set -uo pipefail
export PATH="$HOME/.local/bin:$PATH"

T="/home/vlad/RuView/firmware/esp32-csi-node/tools"   # CSI mesh launchers
CK="/home/vlad/.warp/cockpit"                         # host/aesthetic scripts
CWD="${COCKPIT_CWD:-$T}"
LABEL="MESH-COCKPIT"

command -v herdr >/dev/null 2>&1 || { echo "ERROR: herdr not found in PATH"; exit 1; }

pyid(){ python3 -c 'import sys,json;print(json.load(sys.stdin)["result"]["pane"]["pane_id"])' 2>/dev/null; }
split(){ herdr pane split --pane "$1" --direction "$2" --ratio "$3" --no-focus 2>/dev/null | pyid; }
run(){ herdr pane send-keys "$1" C-c >/dev/null 2>&1; sleep 0.2; herdr pane run "$1" "${@:2}" >/dev/null 2>&1 || true; }
name(){ herdr pane rename "$1" "$2" >/dev/null 2>&1 || true; }

# ── tear down any previous MESH-COCKPIT tab (idempotent re-run) ──
herdr tab list 2>/dev/null | python3 -c '
import sys,json
try: d=json.load(sys.stdin)
except Exception: sys.exit()
for t in d.get("result",{}).get("tabs",[]):
    if t.get("label")=="'"$LABEL"'": print(t["tab_id"])
' 2>/dev/null | while read -r tid; do herdr tab close "$tid" >/dev/null 2>&1 || true; done

# ── new tab ──
read PROOT TID < <(herdr tab create --label "$LABEL" --cwd "$CWD" --focus 2>/dev/null \
  | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["result"]["root_pane"]["pane_id"],d["result"]["tab"]["tab_id"])')
[ -z "${PROOT:-}" ] && { echo "ERROR: could not create cockpit tab (is herdr running?)"; exit 1; }

# ── 3 equal columns ──
CMID=$(split "$PROOT" right 0.3333)
CRIGHT=$(split "$CMID" right 0.5)
CLEFT="$PROOT"

# ── 3 equal rows per column → "top mid bot" ──
rows(){ local top=$1 rest bot; rest=$(split "$top" down 0.3333); bot=$(split "$rest" down 0.5); echo "$top $rest $bot"; }
read L_T L_M L_B < <(rows "$CLEFT")
read M_T M_M M_B < <(rows "$CMID")
read R_T R_M R_B < <(rows "$CRIGHT")
sleep 0.5

# ── wire the 9 panes to live views ──
name "$L_T" health;     run "$L_T" "$T/viz_health.sh"
name "$L_M" rssi;       run "$L_M" "$T/viz_rssi.sh"
name "$L_B" sysmon;     run "$L_B" bash "$CK/sysmon.sh"
name "$M_T" battery;    run "$M_T" "$T/viz_batt.sh"
name "$M_M" csi-amp;    run "$M_M" "$T/viz_amp.sh"
name "$M_B" netmon;     run "$M_B" bash "$CK/netmon.sh"
name "$R_T" aggregator; run "$R_T" "$T/watch_agg.sh"
name "$R_M" services;   run "$R_M" "$T/viz_svc.sh"
name "$R_B" clock;      run "$R_B" bash "$CK/clock.sh"

herdr pane focus "$L_T" >/dev/null 2>&1 || true
echo "✔ MESH-COCKPIT built in tab $TID"
echo "   health  | battery | aggregator"
echo "   rssi    | csi-amp | services"
echo "   sysmon  | netmon  | clock"
echo "   (re-run this script anytime to rebuild; other tabs untouched)"
