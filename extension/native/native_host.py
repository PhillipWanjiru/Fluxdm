#!/usr/bin/env python3
# FluxDM — native_host.py
import sys, json, struct
def read_msg():
    raw = sys.stdin.buffer.read(4)
    if len(raw) < 4: return None
    return json.loads(sys.stdin.buffer.read(struct.unpack("<I",raw)[0]))
def write_msg(d):
    enc=json.dumps(d).encode()
    sys.stdout.buffer.write(struct.pack("<I",len(enc))+enc); sys.stdout.buffer.flush()
while True:
    msg = read_msg()
    if not msg: break
    write_msg({"status":"ok"})
