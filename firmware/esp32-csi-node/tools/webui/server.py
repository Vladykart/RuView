#!/usr/bin/env python3
"""TWPS CSI-mesh web config/status page (host-side, standalone).

Does NOT touch the RuView sensing-server or any serial port. Talks to the ESP
nodes over HTTP (:8032 OTA/config/reboot) and reads the aggregator (:8080).

    python3 server.py [port]        # default 8099
    open http://127.0.0.1:8099/
"""
import json
import os
import subprocess
import sys
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
PSK = os.environ.get("TWPS_OTA_PSK", "dfcd8a101ad40a71d1b2b28e2ee8e923")
OTA_PORT = 8032
AGG = os.environ.get("TWPS_AGG", "http://127.0.0.1:8080")
FW = os.environ.get(
    "TWPS_FW",
    "/home/vlad/RuView/firmware/esp32-csi-node/build-sticks3/esp32-csi-node.bin",
)
KNOWN_IPS = ["192.168.1.135", "192.168.1.16", "192.168.1.61",
             "192.168.1.94", "192.168.1.151", "192.168.1.213"]
MAC_ID = {
    "7c:e8:b1:e1:af:64": 1, "7c:e8:b1:e4:b5:c8": 2, "7c:e8:b1:e2:0e:d0": 3,
    "7c:e8:b1:e2:13:60": 4, "7c:e8:b1:e1:af:74": 5, "7c:e8:b1:e5:41:30": 6,
}


def http_get(url, timeout=3):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except Exception:
        return None


def discover():
    """Return {ip: mac} for ESP nodes (ARP + known IPs)."""
    out = {}
    try:
        neigh = subprocess.run(["ip", "neigh", "show"], capture_output=True,
                               text=True, timeout=4).stdout
        for line in neigh.splitlines():
            p = line.split()
            if len(p) >= 5 and p[0].startswith("192.") and "7c:e8:b1" in line.lower():
                out[p[0]] = p[4].lower()
    except Exception:
        pass
    for ip in KNOWN_IPS:
        out.setdefault(ip, "?")
    return out


def node_status(ip, mac):
    st = http_get(f"http://{ip}:{OTA_PORT}/ota/status")
    nid = MAC_ID.get(mac, None)
    if not st:
        return {"id": nid, "ip": ip, "mac": mac, "online": False}
    batt = st.get("battery_pct", st.get("battery_mv", st.get("battery")))
    return {
        "id": nid, "ip": ip, "mac": mac, "online": True,
        "version": st.get("version", "?"),
        "partition": st.get("running_partition", "?"),
        "mmwave": st.get("mmwave", "none"),
        "battery": batt,  # None until firmware adds it
        "battery_mv": st.get("battery_mv"),
    }


def build_status():
    nodes = []
    for ip, mac in sorted(discover().items(), key=lambda kv: (MAC_ID.get(kv[1], 99), kv[0])):
        nodes.append(node_status(ip, mac))
    agg = http_get(f"{AGG}/api/v1/sensing/latest", timeout=4) or {}
    an = agg.get("nodes") or []
    streaming = {}
    for n in an:
        streaming[n.get("node_id")] = {"rssi": n.get("rssi_dbm"),
                                       "subcarriers": n.get("subcarrier_count")}
    # merge streaming info into nodes
    for nd in nodes:
        s = streaming.get(nd.get("id"))
        if s:
            nd["rssi"] = s["rssi"]
            nd["subcarriers"] = s["subcarriers"]
            nd["streaming"] = True
        else:
            nd["streaming"] = False
    summary = {
        "presence": (agg.get("classification") or {}).get("presence"),
        "motion": (agg.get("classification") or {}).get("motion_level"),
        "persons": agg.get("estimated_persons"),
        "mean_rssi": (agg.get("features") or {}).get("mean_rssi"),
        "streaming_ids": sorted([i for i in streaming.keys() if i is not None]),
        "vitals": agg.get("vital_signs") or {},
    }
    return {"nodes": nodes, "aggregator": summary, "fw": FW}


def post_ota(ip):
    try:
        with open(FW, "rb") as f:
            data = f.read()
    except Exception as e:
        return {"ok": False, "msg": f"firmware read error: {e}"}
    req = urllib.request.Request(
        f"http://{ip}:{OTA_PORT}/ota", data=data, method="POST",
        headers={"Authorization": f"Bearer {PSK}",
                 "Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return {"ok": True, "msg": r.read().decode()[:200]}
    except Exception as e:
        # node often drops the connection as it reboots -> treat as success-ish
        return {"ok": True, "msg": f"pushed (node rebooting): {e}"}


def post_passthrough(ip, path, body):
    """POST JSON to a node endpoint (e.g. /config, /reboot). 404 => needs OTA."""
    data = json.dumps(body).encode() if body is not None else b""
    req = urllib.request.Request(
        f"http://{ip}:{OTA_PORT}{path}", data=data, method="POST",
        headers={"Authorization": f"Bearer {PSK}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return {"ok": True, "msg": r.read().decode()[:300]}
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return {"ok": False, "msg": f"{path} not supported — node needs OTA for web-config"}
        return {"ok": False, "msg": f"HTTP {e.code}: {e.reason}"}
    except Exception as e:
        return {"ok": True, "msg": f"sent (node may be rebooting): {e}"}


INDEX = """<!doctype html><html><head><meta charset=utf-8>
<title>TWPS CSI Mesh</title><meta name=viewport content="width=device-width,initial-scale=1">
<style>
:root{color-scheme:dark}
body{font:14px system-ui,sans-serif;margin:0;background:#0b1016;color:#dbe4ee}
header{padding:12px 18px;background:#11202e;border-bottom:1px solid #1c3347}
h1{font-size:17px;margin:0;color:#4fd6ff;letter-spacing:.5px}
.sub{color:#7aa0bd;font-size:12px;margin-top:3px}
main{padding:16px 18px;max-width:1150px}
.sum{display:flex;gap:18px;flex-wrap:wrap;margin:0 0 16px}
.card{background:#111b26;border:1px solid #1d3346;border-radius:8px;padding:10px 14px}
.card b{color:#9fe6ff}
table{border-collapse:collapse;width:100%;background:#0e1620;border:1px solid #1d3346;border-radius:8px;overflow:hidden}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #16293a;font-size:13px;white-space:nowrap}
th{background:#13273a;color:#8fb6d6;font-weight:600}
tr:last-child td{border-bottom:0}
.on{color:#58e08a}.off{color:#e0595f}.warn{color:#e0b84f}
.pill{display:inline-block;padding:1px 8px;border-radius:10px;font-size:11px}
.pill.y{background:#123a24;color:#58e08a}.pill.n{background:#3a1214;color:#e0595f}
button{font:12px system-ui;background:#18354b;color:#cfe8ff;border:1px solid #2b556f;border-radius:5px;padding:4px 9px;cursor:pointer;margin:1px}
button:hover{background:#1f465f}
button.danger{background:#3a1a1c;border-color:#5f2b2b;color:#ffbcbc}
dialog{background:#101b26;color:#dbe4ee;border:1px solid #27475f;border-radius:10px;width:320px}
dialog input{width:100%;padding:6px;margin:4px 0 10px;background:#0b141d;border:1px solid #27475f;border-radius:5px;color:#dbe4ee}
label{font-size:12px;color:#8fb6d6}
#msg{position:fixed;right:14px;bottom:14px;background:#13273a;border:1px solid #2b556f;border-radius:7px;padding:10px 14px;max-width:420px;display:none}
small{color:#6f93b0}
</style></head><body>
<header><h1>TWPS&nbsp;·&nbsp;WiFi-CSI Mesh Control</h1>
<div class=sub id=hdr>loading…</div></header>
<main>
<div class=sum id=sum></div>
<table><thead><tr>
<th>ID</th><th>IP</th><th>Status</th><th>Firmware</th><th>Slot</th><th>mmWave</th>
<th>Battery</th><th>RSSI</th><th>Streaming</th><th>Actions</th></tr></thead>
<tbody id=rows></tbody></table>
<p><button onclick="otaAll()">⬆ OTA update ALL</button>
<small>firmware: <span id=fw></span></small></p>
</main>
<dialog id=cfg><form method=dialog><h3 style="margin:4px 0 10px;color:#9fe6ff">Configure node <span id=cfgid></span></h3>
<label>SSID</label><input id=c_ssid value="George Salamasov">
<label>Password</label><input id=c_pass type=text value="">
<label>Channel</label><input id=c_chan value="11">
<label>Node ID</label><input id=c_nid>
<label>Target IP (aggregator)</label><input id=c_tip value="192.168.1.59">
<div style="display:flex;gap:8px;justify-content:flex-end">
<button value=cancel>Cancel</button><button id=cfgsave value=save>Save &amp; reboot</button></div>
</form></dialog>
<div id=msg></div>
<script>
let CUR=null;
function toast(t){let m=document.getElementById('msg');m.textContent=t;m.style.display='block';setTimeout(()=>m.style.display='none',6000);}
async function load(){
 let r=await fetch('/api/status');let d=await r.json();
 document.getElementById('fw').textContent=d.fw;
 let a=d.aggregator;
 document.getElementById('hdr').textContent='aggregator '+(a.presence?'● present':'○ empty')+
   ' · persons '+a.persons+' · streaming ids ['+a.streaming_ids.join(', ')+'] · mean RSSI '+a.mean_rssi+' dBm';
 let v=a.vitals||{};let vt=(v.heart_rate_bpm?('HR '+Math.round(v.heart_rate_bpm)+' / BR '+Math.round(v.breathing_rate_bpm||0)):'—');
 document.getElementById('sum').innerHTML=
  '<div class=card><b>Nodes online</b><br>'+d.nodes.filter(n=>n.online).length+' / '+d.nodes.length+'</div>'+
  '<div class=card><b>Streaming</b><br>'+a.streaming_ids.length+' node(s)</div>'+
  '<div class=card><b>Presence</b><br>'+(a.presence?'<span class=on>yes</span>':'<span class=off>no</span>')+' ('+a.motion+')</div>'+
  '<div class=card><b>CSI vitals</b><br>'+vt+'</div>';
 let rows='';
 for(const n of d.nodes){
  let batt=(n.battery==null)?'<span class="warn">— (OTA)</span>':('<span class="'+(n.battery<20?'off':(n.battery<40?'warn':'on'))+'" title="'+(n.battery_mv||'?')+' mV">'+n.battery+'%</span>');
  rows+='<tr><td>'+(n.id??'?')+'</td><td>'+n.ip+'</td>'+
   '<td>'+(n.online?'<span class="pill y">online</span>':'<span class="pill n">offline</span>')+'</td>'+
   '<td>'+(n.version?n.version.split('-g').pop():'—')+'</td>'+
   '<td>'+(n.partition||'—')+'</td><td>'+(n.mmwave||'—')+'</td>'+
   '<td>'+batt+'</td>'+
   '<td>'+(n.rssi!=null?n.rssi+' dBm':'—')+'</td>'+
   '<td>'+(n.streaming?'<span class=on>●</span>':'<span class=off>○</span>')+'</td>'+
   '<td>'+(n.online?('<button onclick="ota(\\''+n.ip+'\\')">OTA</button>'+
     '<button onclick="reboot(\\''+n.ip+'\\')">Reboot</button>'+
     '<button onclick="openCfg(\\''+n.ip+'\\','+(n.id??'')+')">Config</button>'):'')+'</td></tr>';
 }
 document.getElementById('rows').innerHTML=rows;
}
async function ota(ip){if(!confirm('OTA update '+ip+'?'))return;toast('OTA → '+ip+' …');let r=await fetch('/api/ota',{method:'POST',body:JSON.stringify({ip})});toast((await r.json()).msg);}
async function otaAll(){if(!confirm('OTA update ALL nodes?'))return;toast('OTA → all …');let r=await fetch('/api/ota',{method:'POST',body:JSON.stringify({ip:'all'})});toast((await r.json()).msg);}
async function reboot(ip){if(!confirm('Reboot '+ip+'?'))return;let r=await fetch('/api/reboot',{method:'POST',body:JSON.stringify({ip})});toast((await r.json()).msg);}
function openCfg(ip,id){CUR=ip;document.getElementById('cfgid').textContent=id+' ('+ip+')';document.getElementById('c_nid').value=id||'';document.getElementById('cfg').showModal();}
document.getElementById('cfgsave').addEventListener('click',async(e)=>{
 let body={ip:CUR,ssid:c_ssid.value,password:c_pass.value,channel:+c_chan.value,node_id:+c_nid.value,target_ip:c_tip.value};
 toast('config → '+CUR+' …');let r=await fetch('/api/config',{method:'POST',body:JSON.stringify(body)});toast((await r.json()).msg);
});
load();setInterval(load,4000);
</script></body></html>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            self._send(200, INDEX, "text/html; charset=utf-8")
        elif self.path.startswith("/api/status"):
            self._send(200, json.dumps(build_status()))
        else:
            self._send(404, json.dumps({"error": "not found"}))

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            body = {}
        if self.path == "/api/ota":
            ip = body.get("ip")
            if ip == "all":
                res = [{"ip": k, **post_ota(k)} for k in discover().keys()]
                self._send(200, json.dumps({"ok": True, "msg": "OTA pushed to all",
                                            "detail": res}))
            else:
                self._send(200, json.dumps(post_ota(ip)))
        elif self.path == "/api/reboot":
            self._send(200, json.dumps(post_passthrough(body.get("ip"), "/reboot", {})))
        elif self.path == "/api/config":
            ip = body.pop("ip", None)
            self._send(200, json.dumps(post_passthrough(ip, "/config", body)))
        else:
            self._send(404, json.dumps({"error": "not found"}))


if __name__ == "__main__":
    print(f"TWPS mesh web UI  ->  http://127.0.0.1:{PORT}/  (and http://192.168.1.59:{PORT}/)")
    ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
