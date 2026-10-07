#!/usr/bin/env bash
exec python3 - <<'PY'
import urllib.request,json,time,datetime
U="http://127.0.0.1:8080/api/v1/sensing/latest"
BL=" .:-=+*#%@"
while True:
    try:
        d=json.load(urllib.request.urlopen(U,timeout=2))
        ns=d.get("nodes",[])
        print("\033[2J\033[H== CSI amplitude (live frame) ==  "+datetime.datetime.now().strftime("%H:%M:%S"))
        for n in ns[:2]:
            a=n.get("amplitude") or []
            if not a: continue
            mx=max(a) or 1
            spark="".join(BL[min(8,int(v/mx*8))] for v in a[:120])
            print("node %s (rssi %s, %d subc):"%(n.get("node_id"),n.get("rssi_dbm"),len(a)))
            print(spark)
        c=d.get("classification",{})
        print("\npresence=%s persons=%s motion=%s"%(c.get("presence"),d.get("estimated_persons"),c.get("motion_level")))
    except Exception as e:
        print("\033[2J\033[Hwaiting for CSI… ",e)
    time.sleep(1)
PY
