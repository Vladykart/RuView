#!/usr/bin/env bash
# space_init.sh — one command to build the ENTIRE TWPS CSI-mesh herdr workspace.
#
# Rebuilds ONLY our project tabs (idempotent, safe to re-run); never touches
# unrelated tabs or the dev COCKPIT / agent tabs. All panes run live commands
# that read over HTTP (never the serial port).
#
#   SENSING       roster + aggregator                          (2 panes)
#   monitor       roster + aggregator                          (2 panes)
#   CSI-VIZ       health|battery|aggregator / rssi|csi-amp|services (6 panes)
#   MESH-COCKPIT  full 9-pane cockpit (via cockpit_init.sh)
#
#   usage:  tools/space_init.sh     (or: tools/mesh.sh space)
set -uo pipefail
export PATH="$HOME/.local/bin:$PATH"

T="/home/vlad/RuView/firmware/esp32-csi-node/tools"
CWD="${SPACE_CWD:-$T}"
command -v herdr >/dev/null 2>&1 || { echo "ERROR: herdr not found"; exit 1; }

pyid(){ python3 -c 'import sys,json;print(json.load(sys.stdin)["result"]["pane"]["pane_id"])' 2>/dev/null; }
split(){ herdr pane split --pane "$1" --direction "$2" --ratio "$3" --no-focus 2>/dev/null | pyid; }
run(){ herdr pane send-keys "$1" C-c >/dev/null 2>&1; sleep 0.2; herdr pane run "$1" "${@:2}" >/dev/null 2>&1 || true; }
name(){ herdr pane rename "$1" "$2" >/dev/null 2>&1 || true; }

teardown(){  # close every tab with this exact label
  herdr tab list 2>/dev/null | LABEL="$1" python3 -c '
import sys,json,os
try: d=json.load(sys.stdin)
except Exception: sys.exit()
for t in d.get("result",{}).get("tabs",[]):
    if t.get("label")==os.environ["LABEL"]: print(t["tab_id"])
' 2>/dev/null | while read -r tid; do herdr tab close "$tid" >/dev/null 2>&1 || true; done
}

newtab(){  # label -> "root_pane tab_id"
  herdr tab create --label "$1" --cwd "$CWD" --focus 2>/dev/null \
    | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["result"]["root_pane"]["pane_id"],d["result"]["tab"]["tab_id"])'
}

build_2pane(){  # label  topname topcmd  botname botcmd
  teardown "$1"
  read ROOT TID < <(newtab "$1"); [ -z "${ROOT:-}" ] && { echo "  ! $1 failed"; return; }
  BOT=$(split "$ROOT" down 0.5); sleep 0.6
  name "$ROOT" "$2"; run "$ROOT" "$3"
  name "$BOT"  "$4"; run "$BOT"  "$5"
  echo "  ✔ $1 ($TID): $2 / $4"
}

build_csiviz(){  # 3 cols x 2 rows
  teardown "CSI-VIZ"
  read ROOT TID < <(newtab "CSI-VIZ"); [ -z "${ROOT:-}" ] && { echo "  ! CSI-VIZ failed"; return; }
  CMID=$(split "$ROOT" right 0.3333); CRIGHT=$(split "$CMID" right 0.5)
  L_B=$(split "$ROOT" down 0.5); M_B=$(split "$CMID" down 0.5); R_B=$(split "$CRIGHT" down 0.5)
  sleep 0.4
  name "$ROOT"  health;     run "$ROOT"  "$T/viz_health.sh"
  name "$L_B"   rssi;       run "$L_B"   "$T/viz_rssi.sh"
  name "$CMID"  battery;    run "$CMID"  "$T/viz_batt.sh"
  name "$M_B"   csi-amp;    run "$M_B"   "$T/viz_amp.sh"
  name "$CRIGHT" aggregator; run "$CRIGHT" "$T/watch_agg.sh"
  name "$R_B"   services;   run "$R_B"   "$T/viz_svc.sh"
  echo "  ✔ CSI-VIZ ($TID): health|battery|aggregator / rssi|csi-amp|services"
}

echo "Building TWPS CSI-mesh workspace…"
build_2pane SENSING roster "$T/watch_mesh.sh" aggregator "$T/watch_agg.sh"
build_2pane monitor roster "$T/watch_mesh.sh" aggregator "$T/watch_agg.sh"
build_csiviz
echo "  … MESH-COCKPIT:"; "$T/cockpit_init.sh" | sed 's/^/  /'
echo
echo "Workspace ready. Tabs: SENSING, monitor, CSI-VIZ, MESH-COCKPIT"
echo "Re-run tools/space_init.sh anytime to rebuild (other tabs untouched)."
