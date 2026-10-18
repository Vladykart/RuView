#!/usr/bin/env bash
exec python3 - <<'PY'
import urllib.request,json,time,datetime
svcs=[("aggregator   :8080","http://127.0.0.1:8080/api/v1/sensing/latest"),
      ("mesh web UI  :8099","http://127.0.0.1:8099/api/status"),
      ("observatory  :3000","http://127.0.0.1:3000/health"),
      ("observ. proxy:18030","http://127.0.0.1:18030/observatory/observatory.html?live=1"),
      ("2D dashboard :5173","http://127.0.0.1:5173/")]
while True:
    print("\033[2J\033[H== TWPS services ==  "+datetime.datetime.now().strftime("%H:%M:%S"))
    for name,u in svcs:
        try: urllib.request.urlopen(u,timeout=2); print("  [UP]   "+name)
        except Exception: print("  [down] "+name)
    print("\n  web UI     : http://192.168.1.59:8099/")
    print("  observatory: http://100.72.92.31:18030/observatory/observatory.html?live=1")
    time.sleep(5)
PY
