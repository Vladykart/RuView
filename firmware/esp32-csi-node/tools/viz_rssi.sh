#!/usr/bin/env bash
exec python3 - <<'PY'
import urllib.request,json,time,datetime,collections
U="http://127.0.0.1:8080/api/v1/sensing/latest"
cnt=collections.Counter(); rssi={}; subc={}
t0=time.time(); last=0
while True:
    try:
        d=json.load(urllib.request.urlopen(U,timeout=2))
        for n in d.get("nodes",[]):
            i=n.get("node_id"); cnt[i]+=1; rssi[i]=n.get("rssi_dbm"); subc[i]=n.get("subcarrier_count")
    except Exception: pass
    if time.time()-last>=2:
        last=time.time(); el=max(1e-3,time.time()-t0)
        print("\033[2J\033[H== Per-node CSI (RSSI + frames) ==  "+datetime.datetime.now().strftime("%H:%M:%S"))
        print("id   rssi(dBm)  subc   frames   rate/s")
        for i in sorted(k for k in cnt if k is not None):
            print("%-4s %-10s %-6s %-8d %.1f"%(i,rssi.get(i),subc.get(i),cnt[i],cnt[i]/el))
    time.sleep(0.4)
PY
