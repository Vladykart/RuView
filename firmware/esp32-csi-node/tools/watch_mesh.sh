#!/usr/bin/env bash
# Live CSI-mesh roster (node id/ip/fw/partition/mmwave/battery/reach + streaming)
MESH="$(dirname "$0")/mesh.sh"
while true; do
  clear
  echo "== TWPS CSI mesh roster ==  $(date '+%F %H:%M:%S')"
  "$MESH" status
  sleep 8
done
