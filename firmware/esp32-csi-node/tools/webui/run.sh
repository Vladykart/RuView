#!/usr/bin/env bash
# Launch the TWPS CSI-mesh web UI (default port 8099)
exec python3 "$(dirname "$0")/server.py" "${1:-8099}"
