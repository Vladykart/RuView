#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# TWPS WiFi-CSI mesh management CLI (host-side, HTTP only — never touches serial)
#
#   mesh.sh list | status          roster: id, ip, fw, partition, mmwave, batt, streaming
#   mesh.sh ota <ip|all> [bin]     push OTA firmware over WiFi (Bearer PSK)
#   mesh.sh diag <ip>              pretty-print /ota/diag + /ota/status for one node
#
# Nodes: M5StickS3 CSI nodes on WiFi "George Salamasov" ch11, OTA server on :8032,
# streaming CSI to the aggregator (RuView sensing-server) at 127.0.0.1:8080.
# ---------------------------------------------------------------------------
set -uo pipefail

PSK="${TWPS_OTA_PSK:-dfcd8a101ad40a71d1b2b28e2ee8e923}"
OTA_PORT=8032
AGG="${TWPS_AGG:-http://127.0.0.1:8080}"
FW_DEFAULT="${TWPS_FW:-/home/vlad/RuView/firmware/esp32-csi-node/build-sticks3/esp32-csi-node.bin}"
KNOWN_IPS="192.168.1.135 192.168.1.16 192.168.1.61 192.168.1.94 192.168.1.151 192.168.1.213"

# MAC -> node id (from provisioning; MACs are stable, DHCP IPs are not)
mac_to_id() {
  case "${1,,}" in
    7c:e8:b1:e1:af:64) echo 1;; 7c:e8:b1:e4:b5:c8) echo 2;;
    7c:e8:b1:e2:0e:d0) echo 3;; 7c:e8:b1:e2:13:60) echo 4;;
    7c:e8:b1:e1:af:74) echo 5;; 7c:e8:b1:e5:41:30) echo 6;;
    *) echo "?";;
  esac
}

discover() {
  # Print "ip mac" for every ESP node: ARP entries + known IPs (deduped).
  { ip neigh show 2>/dev/null | awk '/7c:e8:b1/{print $1, $5}'
    for ip in $KNOWN_IPS; do
      m=$(ip neigh show "$ip" 2>/dev/null | awk '{print $5}')
      echo "$ip ${m:-?}"
    done
  } | awk 'NF>=1 && $1 ~ /^192\./ {print $1, tolower($2)}' | sort -u -k1,1
}

cmd_list() {
  # refresh ARP (quick, quiet) so IPs/MACs are current
  for ip in $KNOWN_IPS; do ping -c1 -W1 "$ip" >/dev/null 2>&1 & done; wait 2>/dev/null

  printf "%-4s %-16s %-7s %-9s %-8s %-6s %-9s\n" ID IP FW PART MMWAVE BATT REACH
  printf -- "---- ---------------- ------- --------- -------- ------ ---------\n"
  while read -r ip mac; do
    [ -z "$ip" ] && continue
    id=$(mac_to_id "$mac")
    js=$(curl -s -m3 "http://$ip:$OTA_PORT/ota/status" 2>/dev/null)
    if [ -z "$js" ]; then
      printf "%-4s %-16s %-7s %-9s %-8s %-6s %-9s\n" "$id" "$ip" "-" "-" "-" "-" "NO"
      continue
    fi
    read -r fw part mmw batt < <(printf '%s' "$js" | python3 -c '
import sys,json
try: d=json.load(sys.stdin)
except Exception: print("? ? ? -"); sys.exit()
fw=(d.get("version") or "?").split("-g")[-1][:7] or "?"
part=d.get("running_partition","?").replace("ota_","o")
mmw=d.get("mmwave","?")
# battery: populated once new firmware adds it; try several likely keys
b=d.get("battery_pct", d.get("batt_pct", d.get("battery", d.get("batt"))))
batt=(str(round(float(b)))+"%") if b not in (None,"") else "-"
print(fw,part,mmw,batt)
')
    printf "%-4s %-16s %-7s %-9s %-8s %-6s %-9s\n" "$id" "$ip" "${fw:-?}" "${part:-?}" "${mmw:-?}" "${batt:--}" "yes"
  done < <(discover)

  echo
  echo "Aggregator ($AGG) — streaming now:"
  curl -s -m4 "$AGG/api/v1/sensing/latest" 2>/dev/null | python3 -c "
import sys,json
try: d=json.load(sys.stdin)
except Exception: print('  (aggregator unreachable or no data)'); sys.exit()
ns=d.get('nodes') or []
if not ns: print('  (no nodes streaming / no data yet)'); sys.exit()
c=d.get('classification',{}); f=d.get('features',{})
for n in ns:
    print('  node %s: rssi %s dBm, %s subcarriers, pos %s' % (n.get('node_id'), n.get('rssi_dbm'), n.get('subcarrier_count'), n.get('position')))
print('  presence=%s persons=%s mean_rssi=%s motion=%s' % (c.get('presence'), d.get('estimated_persons'), f.get('mean_rssi'), c.get('motion_level')))
" 2>/dev/null || echo "  (parse error)"
}

cmd_ota() {
  local target="${1:-}"; local bin="${2:-$FW_DEFAULT}"
  [ -z "$target" ] && { echo "usage: mesh.sh ota <ip|all> [bin]"; exit 2; }
  [ -f "$bin" ] || { echo "firmware not found: $bin"; exit 2; }
  local ips
  if [ "$target" = "all" ]; then ips=$(discover | awk '{print $1}'); else ips="$target"; fi
  for ip in $ips; do
    echo "== OTA -> $ip =="
    curl -s -m180 -X POST "http://$ip:$OTA_PORT/ota" \
      -H "Authorization: Bearer $PSK" -H "Content-Type: application/octet-stream" \
      --data-binary "@$bin" 2>&1 | head -c 200
    echo " (node verifies, reboots to alt slot, auto-rollback on health-gate fail)"
  done
}

cmd_diag() {
  local ip="${1:-}"; [ -z "$ip" ] && { echo "usage: mesh.sh diag <ip>"; exit 2; }
  echo "== $ip /ota/status =="; curl -s -m4 "http://$ip:$OTA_PORT/ota/status" 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "unreachable"
  echo "== $ip /ota/diag =="; curl -s -m4 "http://$ip:$OTA_PORT/ota/diag" 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "unreachable"
}

case "${1:-list}" in
  list|status) cmd_list;;
  ota)   shift; cmd_ota "$@";;
  diag)  shift; cmd_diag "$@";;
  web)   shift; exec "$(dirname "$(readlink -f "$0")")/webui/run.sh" "$@";;
  cockpit) exec "$(dirname "$(readlink -f "$0")")/cockpit_init.sh";;
  space)   exec "$(dirname "$(readlink -f "$0")")/space_init.sh";;
  *) echo "usage: mesh.sh {list|status|ota <ip|all> [bin]|diag <ip>|web [port]}"; exit 2;;
esac
