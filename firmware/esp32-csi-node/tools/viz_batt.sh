#!/usr/bin/env bash
exec python3 - <<'PY'
import urllib.request,json,time,datetime
ids={"192.168.1.135":1,"192.168.1.16":2,"192.168.1.61":3,"192.168.1.94":4,"192.168.1.151":5,"192.168.1.213":6}
while True:
    print("\033[2J\033[H== TWPS node battery ==  "+datetime.datetime.now().strftime("%H:%M:%S"))
    for ip,nid in sorted(ids.items(),key=lambda x:x[1]):
        try:
            d=json.load(urllib.request.urlopen("http://%s:8032/ota/status"%ip,timeout=2))
            p=d.get("battery_pct"); mv=d.get("battery_mv"); pi=int(p)
            bar="#"*(pi//5)+"."*(20-pi//5); flag="  *** LOW ***" if pi<20 else ""
            print("n%d %-15s [%s] %3d%%  %smV%s"%(nid,ip,bar,pi,mv,flag))
        except Exception:
            print("n%d %-15s  offline"%(nid,ip))
    time.sleep(10)
PY
