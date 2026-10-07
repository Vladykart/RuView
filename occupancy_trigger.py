#!/usr/bin/env python3
"""
Simple occupancy trigger for the RuView WiFi-CSI mesh.

Watches the sensing server's live stream and fires an action when the room
becomes OCCUPIED or VACANT. Uses hysteresis so it doesn't flap: it declares
OCCUPIED quickly, but waits a while of continuous no-presence before VACANT
(a person sitting dead-still can momentarily read as no-presence).

Honest scope: this is presence/occupancy only — the one thing the single-antenna
boards do reliably. No pose, no counting, no identity.

Actions (all optional, edit the CONFIG block or set env vars):
  - prints every transition with a timestamp
  - writes current state to a JSON file other tools can poll
  - runs a shell command on each transition (e.g. toggle a smart plug)
  - desktop notification via notify-send if available

Run:  python3 occupancy_trigger.py
"""
import asyncio, json, os, time, subprocess, shutil, sys

# ── CONFIG ──────────────────────────────────────────────────────────────────
WS_URL          = os.environ.get("OCC_WS", "ws://localhost:3000/ws/sensing")
OCCUPIED_AFTER  = float(os.environ.get("OCC_ON_SEC",  "2"))    # presence this long -> OCCUPIED
VACANT_AFTER    = float(os.environ.get("OCC_OFF_SEC", "45"))   # no-presence this long -> VACANT
STATE_FILE      = os.environ.get("OCC_STATE_FILE", "/tmp/occupancy_state.json")
ON_OCCUPIED_CMD = os.environ.get("OCC_ON_OCCUPIED", "")        # shell cmd when room becomes occupied
ON_VACANT_CMD   = os.environ.get("OCC_ON_VACANT",   "")        # shell cmd when room becomes vacant
NOTIFY          = os.environ.get("OCC_NOTIFY", "1") == "1"     # desktop notification
# ────────────────────────────────────────────────────────────────────────────

def ts():
    return time.strftime("%H:%M:%S")

def notify(title, body):
    if NOTIFY and shutil.which("notify-send"):
        try: subprocess.Popen(["notify-send", title, body])
        except Exception: pass

def write_state(occupied, conf):
    try:
        with open(STATE_FILE, "w") as f:
            json.dump({"occupied": occupied, "confidence": round(conf, 2),
                       "updated": time.time()}, f)
    except Exception: pass

def run_hook(cmd, label):
    if cmd:
        print(f"    -> running {label} hook: {cmd}")
        try: subprocess.Popen(cmd, shell=True)
        except Exception as e: print(f"    hook failed: {e}")

def on_transition(occupied, conf):
    if occupied:
        print(f"[{ts()}] 🟢 ROOM OCCUPIED  (confidence {conf:.2f})")
        notify("Room occupied", "WiFi sensing detected presence")
        run_hook(ON_OCCUPIED_CMD, "occupied")
    else:
        print(f"[{ts()}] ⚫ ROOM VACANT")
        notify("Room vacant", "No presence for a while")
        run_hook(ON_VACANT_CMD, "vacant")
    write_state(occupied, conf)

async def run():
    try:
        import websockets
    except ImportError:
        sys.exit("pip install websockets  (needed for the trigger)")

    occupied = None                 # unknown until first decision
    last_present = 0.0              # last time we saw presence
    first_present = None           # when presence began (for OCCUPIED_AFTER)
    print(f"[{ts()}] occupancy trigger watching {WS_URL}")
    print(f"         OCCUPIED after {OCCUPIED_AFTER}s presence, "
          f"VACANT after {VACANT_AFTER}s absence")
    while True:
        try:
            async with websockets.connect(WS_URL) as ws:
                while True:
                    d = json.loads(await ws.recv())
                    cls = d.get("classification", {})
                    present = bool(cls.get("presence"))
                    conf = float(cls.get("confidence", 0.0) or 0.0)
                    now = time.time()

                    if present:
                        last_present = now
                        if first_present is None:
                            first_present = now
                    else:
                        first_present = None

                    # decide state with hysteresis
                    if occupied in (None, False):
                        if present and first_present and (now - first_present) >= OCCUPIED_AFTER:
                            occupied = True
                            on_transition(True, conf)
                        elif occupied is None and not present and (now - last_present) > VACANT_AFTER:
                            occupied = False
                            on_transition(False, conf)
                    if occupied in (None, True):
                        if (now - last_present) >= VACANT_AFTER:
                            if occupied is not False:
                                occupied = False
                                on_transition(False, conf)
        except Exception as e:
            print(f"[{ts()}] stream dropped ({e}); reconnecting in 3s")
            await asyncio.sleep(3)

if __name__ == "__main__":
    asyncio.run(run())
