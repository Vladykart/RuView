#!/usr/bin/env bash
exec python3 - <<'PY'
import urllib.request,json,time,datetime
ids={"192.168.1.135":1,"192.168.1.16":2,"192.168.1.61":3,"192.168.1.94":4,"192.168.1.151":5,"192.168.1.213":6}
AU="http://127.0.0.1:8080/api/v1/sensing/latest"
while True:
    try:
        ag=json.load(urllib.request.urlopen(AU,timeout=2)); streaming=set(n.get("node_id") for n in ag.get("nodes",[]))
    except Exception: streaming=set()
    print("\033[2J\033[H== Node health == "+datetime.datetime.now().strftime("%H:%M:%S"))
    print("id  ip    st   batt strm")
    up=0
    for ip,nid in sorted(ids.items(),key=lambda x:x[1]):
        try:
            d=json.load(urllib.request.urlopen("http://%s:8032/ota/status"%ip,timeout=2))
            b=d.get("battery_pct"); strm="YES" if nid in streaming else " - "; up+=1
            print("n%d .%-4s UP  %3s%%  %s"%(nid,ip.split('.')[-1],b,strm))
        except Exception:
            print("n%d .%-4s off   -    - "%(nid,ip.split('.')[-1]))
    print("online %d/6  streaming %d"%(up,len(streaming)))
    time.sleep(8)
PY
