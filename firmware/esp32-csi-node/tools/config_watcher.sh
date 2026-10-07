#!/usr/bin/env bash
# Auto-finish: OTA the latest firmware (adds /config) to the remaining nodes
# the moment their battery crosses 25%. Idempotent; exits when all done.
FW=/home/vlad/RuView/firmware/esp32-csi-node/build-sticks3/esp32-csi-node.bin
PSK=dfcd8a101ad40a71d1b2b28e2ee8e923
LOG=/home/vlad/RuView/firmware/esp32-csi-node/tools/config_watcher.log
declare -A T=( [1]=192.168.1.135 [2]=192.168.1.16 [6]=192.168.1.213 )
echo "$(date +%T) watcher start; pending nodes: ${!T[*]}" > "$LOG"
for round in $(seq 1 240); do   # ~4h @ 60s
  [ ${#T[@]} -eq 0 ] && { echo "$(date +%T) ALL DONE" >> "$LOG"; break; }
  for n in "${!T[@]}"; do
    ip=${T[$n]}
    pct=$(curl -s -m4 "http://$ip:8032/ota/status" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('battery_pct',-1))" 2>/dev/null)
    [ -z "$pct" ] && pct=-1
    echo "$(date +%T) node$n $ip batt=${pct}%" >> "$LOG"
    if [ "$pct" -ge 25 ] 2>/dev/null; then
      r=$(curl -s -m120 -X POST "http://$ip:8032/ota" -H "Authorization: Bearer $PSK" -H "Content-Type: application/octet-stream" --data-binary "@$FW" 2>&1)
      echo "$(date +%T) node$n >=25% -> OTA: ${r:0:70}" >> "$LOG"
      unset 'T[$n]'
    fi
  done
  sleep 60
done
echo "$(date +%T) watcher end; remaining: ${!T[*]}" >> "$LOG"
