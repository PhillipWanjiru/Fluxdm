#!/usr/bin/env python3
"""
Mock fluxdm-core server for testing CoreIPCClient.java wire protocol.
Implements the exact protocol validated in Phase 1's socket_server.c:
  - Accepts CMD_START, decodes flux_cmd_start_t (56-byte fixed + url/path)
  - Replies RSP_OK
  - Sends 3x RSP_PROGRESS frames
  - Sends RSP_COMPLETE
  - Accepts CMD_PAUSE/RESUME/CANCEL -> replies RSP_OK
  - Accepts CMD_SET_LIMIT -> replies RSP_OK
  - Accepts CMD_STATUS -> replies RSP_STATUS
  - On unknown/bad CMD_START (segments==0) -> replies RSP_ERROR
"""
import socket, struct, os, sys, time, threading

SOCK_PATH = "/tmp/fluxdm_mock.sock"
HDR_FMT = "<IBBHII"
HDR_SIZE = struct.calcsize(HDR_FMT)
MAGIC = 0x464C5558

CMD_START=0x01; CMD_PAUSE=0x02; CMD_RESUME=0x03; CMD_CANCEL=0x04
CMD_SET_LIMIT=0x05; CMD_STATUS=0x06; CMD_STATUS_ALL=0x07; CMD_SHUTDOWN=0xFF
RSP_OK=0x10; RSP_ERROR=0x11; RSP_PROGRESS=0x12; RSP_COMPLETE=0x13; RSP_STATUS=0x14

def send_frame(conn, typ, id_, payload=b""):
    hdr = struct.pack(HDR_FMT, MAGIC, 1, typ, 0, id_, len(payload))
    conn.sendall(hdr + payload)

def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk: return None
        buf += chunk
    return buf

def handle_client(conn):
    results = {"start_decoded": None, "commands_seen": []}
    try:
        while True:
            raw = recv_exact(conn, HDR_SIZE)
            if raw is None: break
            magic, ver, typ, flags, id_, plen = struct.unpack(HDR_FMT, raw)
            if magic != MAGIC:
                print(f"MOCK: bad magic {magic:#x}"); break

            payload = recv_exact(conn, plen) if plen else b""
            results["commands_seen"].append(typ)

            if typ == CMD_START:
                # flux_cmd_start_t fixed part: I H H Q B 32s 7s = 56 bytes
                fixed = struct.unpack_from("<IHHQB32s7s", payload)
                dl_id, segments, flags_f, bw_limit, cksum_type, cksum, pad = fixed
                off = 56
                url_len = struct.unpack_from("<H", payload, off)[0]; off += 2
                url = payload[off:off+url_len].decode(); off += url_len
                path_len = struct.unpack_from("<H", payload, off)[0]; off += 2
                path = payload[off:off+path_len].decode(); off += path_len

                print(f"MOCK: CMD_START id={dl_id} segs={segments} bw={bw_limit} "
                      f"cksum_type={cksum_type} url={url!r} path={path!r}")
                results["start_decoded"] = {
                    "id": dl_id, "segments": segments, "bw_limit": bw_limit,
                    "cksum_type": cksum_type, "url": url, "path": path,
                }

                if segments == 0:
                    err_payload = struct.pack("<B3s124s", 1, b"\x00"*3,
                                              b"invalid segment count" + b"\x00"*102)
                    send_frame(conn, RSP_ERROR, id_, err_payload)
                    continue

                send_frame(conn, RSP_OK, id_)

                # Simulate 3 progress frames then complete
                total = 1048576
                for i in range(1, 4):
                    downloaded = total * i // 4
                    prog = struct.pack("<IQQIIB3s", id_, total, downloaded,
                                       2_000_000, 1, 4, b"\x00"*3)
                    send_frame(conn, RSP_PROGRESS, id_, prog)
                    time.sleep(0.05)

                comp = struct.pack("<IQIB32s3s", id_, total, 500, 1,
                                   b"\xAB"*32, b"\x00"*3)
                send_frame(conn, RSP_COMPLETE, id_, comp)

            elif typ in (CMD_PAUSE, CMD_RESUME, CMD_CANCEL):
                send_frame(conn, RSP_OK, id_)

            elif typ == CMD_SET_LIMIT:
                (bps,) = struct.unpack_from("<Q", payload)
                print(f"MOCK: CMD_SET_LIMIT id={id_} bps={bps}")
                send_frame(conn, RSP_OK, id_)

            elif typ == CMD_STATUS:
                st = struct.pack("<IBBBBQQII", id_, 2, 4, 4, 0,
                                 1048576, 524288, 3_000_000, 10)
                send_frame(conn, RSP_STATUS, id_, st)

            elif typ == CMD_STATUS_ALL:
                send_frame(conn, RSP_OK, 0)

            elif typ == CMD_SHUTDOWN:
                send_frame(conn, RSP_OK, 0)
                break

        return results
    finally:
        conn.close()

if __name__ == "__main__":
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK_PATH)
    srv.listen(1)
    print(f"MOCK: listening on {SOCK_PATH}")
    print("READY", flush=True)

    conn, _ = srv.accept()
    print("MOCK: client connected")
    handle_client(conn)
    srv.close()
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)
    print("MOCK: done")
