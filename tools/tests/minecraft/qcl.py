#!/usr/bin/env python3
"""qcl.py SOCK [W H] cmd... - drive a QEMU over a second QMP socket.
cmds: shot:NAME (-> /tmp/NAME.png)  c:X,Y (left click)  r:X,Y  m:X,Y (move)
      k:qcode[+qcode]  t:text  w:SECONDS  hold:qcode:SECONDS"""
import json, socket, subprocess, sys, time

sock_path = sys.argv[1]
args = sys.argv[2:]
W, H = 800, 600
if len(args) >= 2 and args[0].isdigit() and args[1].isdigit():
    W, H = int(args[0]), int(args[1]); args = args[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
f = s.makefile("rw")
f.readline()
def cmd(ex, **a):
    f.write(json.dumps({"execute": ex, "arguments": a} if a else {"execute": ex}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "event" not in r: return r
cmd("qmp_capabilities")
def move(x, y):
    cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": x * 32767 // W}},
                                     {"type": "abs", "data": {"axis": "y", "value": y * 32767 // H}}])
def btn(b, d): cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": b}}])
def key(ks, hold=0.08):
    ev = [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}} for k in ks]
    cmd("input-send-event", events=ev); time.sleep(hold)
    ev = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}} for k in reversed(ks)]
    cmd("input-send-event", events=ev); time.sleep(0.08)
for a in args:
    if a.startswith("shot:"):
        n = a[5:]
        print(cmd("screendump", filename="/tmp/%s.ppm" % n))
        subprocess.run(["python3", "/home/banana/bos-tests/ppm2png.py", "/tmp/%s.ppm" % n, "/tmp/%s.png" % n])
    elif a[:2] in ("c:", "r:", "m:"):
        x, y = map(int, a[2:].split(","))
        move(x, y); time.sleep(0.3)
        if a[0] != "m":
            b = "left" if a[0] == "c" else "right"
            btn(b, True); time.sleep(0.15); btn(b, False); time.sleep(0.5)
    elif a.startswith("k:"): key(a[2:].split("+"))
    elif a.startswith("hold:"):
        _, k, t = a.split(":"); key([k], float(t))
    elif a.startswith("t:"):
        for ch in a[2:]:
            if ch == " ": key(["spc"])
            elif ch.isupper(): key(["shift", ch.lower()])
            else: key([ch])
    elif a.startswith("w:"): time.sleep(float(a[2:]))
