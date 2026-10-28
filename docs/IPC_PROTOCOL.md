# IPC protocol, version 1

The wire protocol between `fluxdm-core` and its clients (the Java daemon, the test harnesses, or any
program you write). The definitions live in [`ipc/protocol.h`](../ipc/protocol.h) and
[`ipc/messages.h`](../ipc/messages.h); the core's encoder and incremental decoder are in
[`core/src/ipc/frame_io.c`](../core/src/ipc/frame_io.c), the server in
[`core/src/ipc/socket_server.c`](../core/src/ipc/socket_server.c), and the daemon's client is
[`CoreIPCClient.java`](../daemon/src/main/java/com/fluxdm/ipc/CoreIPCClient.java). This document
describes what those files do.

- [Transport](#transport)
- [Framing](#framing)
- [Commands](#commands)
- [Responses and events](#responses-and-events)
- [Payload layouts](#payload-layouts)
- [Error codes](#error-codes)
- [Ordering and delivery guarantees](#ordering-and-delivery-guarantees)
- [Limits and back-pressure](#limits-and-back-pressure)
- [A complete client in 60 lines](#a-complete-client-in-60-lines)
- [Compatibility](#compatibility)

## Transport

- A Unix-domain stream socket (`AF_UNIX`, `SOCK_STREAM`), created with mode **`0600`**.
- Default path `/tmp/fluxdm.sock` (`FLUX_IPC_SOCK_PATH`); override with `fluxdm-core --sock <path>`. The
  daemon uses `/tmp/fluxdm-<user>.sock` and passes it to the core, so the two always agree. The path
  must be shorter than 108 bytes (`sizeof(sockaddr_un.sun_path)`).
- The server refuses to take over a socket that a live process is serving, replaces a stale socket
  file left by a crash, and never deletes a path that is not a socket.
- Up to **16** clients at once (`FLUX_IPC_MAX_CLIENTS`). A 17th connection is closed immediately.
- **All multi-byte integers are little-endian.** All structs are packed (no padding other than the
  explicit `_pad` fields).

## Framing

Every message, in both directions, is a 16-byte header followed by `payload_len` bytes.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x464C5558`. Little-endian on the wire, so the bytes are `58 55 4C 46` (`"XULF"` in a hex dump). |
| 4 | 1 | `version` | Protocol version, currently `1`. |
| 5 | 1 | `type` | Message type (below). |
| 6 | 2 | `flags` | Bit 0 = `FLUX_FLAG_EVENT`. All other bits are `0`. |
| 8 | 4 | `id` | Download id. `0` means "global" (daemon-wide commands). |
| 12 | 4 | `payload_len` | Number of payload bytes that follow. At most 1 MiB (`FLUX_MAX_PAYLOAD`). |

A frame with a wrong `magic`, an unsupported `version` or an oversized `payload_len` cannot be
resynchronised. The server logs it and **disconnects that client**; the core and every other client carry
on. An *unknown command type*, by contrast, is answered with an `ERROR` frame (code 99) and the
connection stays up.

A reader must handle frames split across reads and several frames in one read; the core's decoder is
tested against both, plus oversized and garbage input.

## Commands

Client → core. `id` is the download id the client chooses for `START` and refers to in the others.

| Type | Code | Payload | Success reply | Notes |
|---|---:|---|---|---|
| `START` | `0x01` | `flux_cmd_start_t` + URL + path | `OK` (id), then events | Id `0` is reserved. An id whose worker is still running is refused; a finished, failed or paused download with the same id is replaced and its partial file is picked up through its resume state. `http` and `https` URLs only. Acknowledged **before** the download is launched, so the `OK` always precedes its events. |
| `PAUSE` | `0x02` | none | `OK` | Flushes buffers, closes connections, writes the checkpoint. Waits up to ~2 s for the worker. |
| `RESUME` | `0x03` | none | `OK` | Valid for paused and failed downloads. Relaunches the worker, which re-probes and continues from the checkpoint. An immediate resume withdraws a pause that has not taken effect yet. |
| `CANCEL` | `0x04` | none | `OK` | **Idempotent**: cancelling something that is already gone succeeds. Deletes the partial file and resume state; a finished download's output is never touched. |
| `SET_LIMIT` | `0x05` | `flux_cmd_set_limit_t` | `OK` | `id` `0` sets the daemon-wide limit, any other id that download's limit. `0` bytes/s = unlimited. Takes effect immediately. |
| `STATUS` | `0x06` | none | `STATUS` | One download's snapshot. |
| `STATUS_ALL` | `0x07` | none | zero or more `STATUS`, then `OK` (id 0) | The terminating `OK` is how a client knows the list is complete. Finished downloads stay listed for 10 minutes. |
| `SHUTDOWN` | `0xFF` | none | `OK` (id 0), then the core stops | Partial files and resume state are kept. |

`PAUSE`, `RESUME`, `STATUS` and `SET_LIMIT` on an unknown id are answered with `ERROR` / `NOT_FOUND`.

## Responses and events

Core → client.

| Type | Code | Payload | Kind |
|---|---:|---|---|
| `OK` | `0x10` | none | reply |
| `ERROR` | `0x11` | `flux_rsp_error_t` | reply **or** event |
| `PROGRESS` | `0x12` | `flux_rsp_progress_t` | event, droppable |
| `COMPLETE` | `0x13` | `flux_rsp_complete_t` | event |
| `STATUS` | `0x14` | `flux_rsp_status_t` | reply |

**Replies and events.** The core pushes frames on its own initiative (progress, completion and
asynchronous failures) and marks them with `FLUX_FLAG_EVENT`. A reply to a command never carries the
flag. This matters because "the answer to my `PAUSE 7`" and "download 7 just failed" are both `ERROR`
frames with id 7; the flag is the only thing that tells them apart.

**Events are broadcast** to every connected client. There is no per-download subscription table to go
stale when a client reconnects: after (re)connecting, send `STATUS_ALL` to learn the current state.

`PROGRESS` is emitted every 250 ms per running download. `COMPLETE` carries the verified checksum
state. An asynchronous `ERROR` ends the download; whatever was already on disk, with its resume state, is
kept (except after a checksum mismatch), so the download can be resumed.

## Payload layouts

Offsets are in bytes from the start of the payload. All fields little-endian.

### `START` (`flux_cmd_start_t`, 56 bytes, then two length-prefixed strings)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `id` | Ignored; the header's `id` is authoritative. |
| 4 | 2 | `segments` | Requested connections. `0` = default (8); values above 64 are capped at 64. The core may use fewer: one segment per 256 KiB of file, and one if the server cannot do ranges. |
| 6 | 2 | `flags` | Reserved, send `0`. |
| 8 | 8 | `bandwidth_limit` | Bytes per second for this download. `0` = unlimited. |
| 16 | 1 | `checksum_type` | `0` none, `1` MD5, `2` SHA-256. |
| 17 | 32 | `checksum` | Expected digest (MD5 uses the first 16 bytes). Ignored when `checksum_type` is `0`. |
| 49 | 7 | padding | Send zeros. |
| 56 | 2 | `url_len` | Then `url_len` bytes of URL. |
| 58 + `url_len` | 2 | `path_len` | Then `path_len` bytes of absolute destination path. |

The core checks every length against the frame. The URL must be non-empty and shorter than 4096 bytes
(`FLUX_MAX_URL`); the path non-empty, shorter than 1024 (`FLUX_MAX_PATH`) and not ending in `/`. Neither
string may contain a NUL byte; they are refused rather than silently truncated. Missing parent
directories of the destination are created.

### `SET_LIMIT` (`flux_cmd_set_limit_t`, 8 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `bytes_per_sec` |

### `ERROR` (`flux_rsp_error_t`, 128 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 1 | `code` | See [Error codes](#error-codes). |
| 1 | 3 | padding | |
| 4 | 124 | `message` | NUL-terminated UTF-8. Cut at a character boundary if the text is longer. |

### `PROGRESS` (`flux_rsp_progress_t`, 32 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `id` | |
| 4 | 8 | `total_bytes` | `0` while unknown. |
| 12 | 8 | `downloaded_bytes` | Bytes received so far. |
| 20 | 4 | `speed_bps` | Rolling average, bytes per second. |
| 24 | 4 | `eta_seconds` | `0xFFFFFFFF` = unknown. |
| 28 | 1 | `active_segments` | Connections currently transferring. |
| 29 | 1 | `status` | `flux_download_status_t`, see below. |
| 30 | 2 | padding | |

### `COMPLETE` (`flux_rsp_complete_t`, 52 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `id` | |
| 4 | 8 | `total_bytes` | |
| 12 | 4 | `elapsed_ms` | Saturates at `0xFFFFFFFF`. |
| 16 | 1 | `checksum_ok` | `1` if a checksum was supplied and matched. |
| 17 | 32 | `checksum` | The computed digest. |
| 49 | 3 | padding | |

A download whose checksum does **not** match never produces `COMPLETE`: it ends in an asynchronous
`ERROR` (code 10) and the file is deleted.

### `STATUS` (`flux_rsp_status_t`, 32 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `id` | |
| 4 | 1 | `status` | See below. |
| 5 | 1 | `active_segments` | |
| 6 | 1 | `total_segments` | |
| 7 | 1 | padding | |
| 8 | 8 | `total_bytes` | |
| 16 | 8 | `downloaded_bytes` | |
| 24 | 4 | `speed_bps` | |
| 28 | 4 | `eta_seconds` | `0xFFFFFFFF` = unknown. |

`status` values: `0` idle, `1` connecting, `2` downloading, `3` paused, `4` complete, `5` error,
`6` cancelled.

The sizes above are asserted at compile time in the core (`_Static_assert` in `socket_server.c`):
a layout change breaks the build, not production.

## Error codes

Carried in `flux_rsp_error_t.code`. The core's finer-grained internal codes are folded into these:

| Code | Name | Raised when |
|---:|---|---|
| 0 | `NONE` | |
| 1 | `INVALID_URL` | Malformed URL, unsupported scheme, empty or oversized URL |
| 2 | `NETWORK` | Connection refused or reset, DNS failure, malformed HTTP, too many redirects, other socket errors |
| 3 | `TIMEOUT` | Connect or receive timeout |
| 4 | `DISK` | Cannot open, write, read or seek the file; not enough space |
| 5 | `TLS` | Handshake failure, certificate or hostname verification failure |
| 6 | `NOT_FOUND` | HTTP 404 / 410; also "no such download" for commands on an unknown id |
| 7 | `FORBIDDEN` | HTTP 401 / 403 |
| 8 | `SERVER` | HTTP 5xx and other unexpected statuses, after retries (and, for 429 / 503, after the patience ran out) |
| 9 | `RANGE` | HTTP 416, a range request answered wrongly, or the remote file changed size |
| 10 | `CHECKSUM` | The downloaded file does not match the supplied digest |
| 99 | `INTERNAL` | Malformed command payload, unknown command, duplicate or reserved id, wrong state for the command, out of memory |

## Ordering and delivery guarantees

1. **Replies are in command order, per connection.** One thread reads and answers a connection's
   commands, so the *n*-th reply belongs to the *n*-th command. Clients can match replies by position
   instead of keeping request ids (the Java client does exactly that).
2. **A `START`'s `OK` precedes every event of that download** on the same connection.
3. **Events of one download arrive in the order they were produced.** Events of different downloads may
   interleave, and an event can arrive between a command and its reply. Use the `EVENT` flag to tell
   them apart.
4. **`PROGRESS` is lossy by design.** The next one supersedes it, so it is dropped for a client that
   has fallen behind. `COMPLETE` and `ERROR` are never dropped (the client is disconnected instead).
5. **Frames never interleave mid-frame**, even though several threads produce them: each is appended
   whole to the client's output queue under that client's lock.
6. **`CANCEL` is idempotent and `STATUS_ALL` is terminated**, so a client can always tell when it has
   the whole picture.
7. **A command never blocks on a slow peer.** Output goes through a per-client queue that is drained
   when the socket becomes writable.

## Limits and back-pressure

| Limit | Value | Constant | On exceeding |
|---|---|---|---|
| Payload size | 1 MiB | `FLUX_MAX_PAYLOAD` | Client disconnected |
| Clients | 16 | `FLUX_IPC_MAX_CLIENTS` | New connection closed |
| Queued output per client, soft | 256 KiB | `FLUX_IPC_OUT_SOFT` | `PROGRESS` frames are dropped |
| Queued output per client, hard | 8 MiB | `FLUX_IPC_OUT_HARD` | Client disconnected |
| URL | < 4096 bytes | `FLUX_MAX_URL` | `ERROR` / `INVALID_URL` |
| Destination path | < 1024 bytes | `FLUX_MAX_PATH` | `ERROR` |
| Segments per download | 64 | `FLUX_MAX_SEGMENTS` | Capped |

A client that stops reading therefore costs the core at most 8 MiB and never slows a download.

## A complete client in 60 lines

Standard library only. It starts one download and follows it to the end. It is tested against the real
core; the offsets above are exactly what its `struct` formats encode.

```python
#!/usr/bin/env python3
"""Minimal FluxDM IPC client (protocol v1): start one download and follow it to the end.

    python3 ipc_client.py /tmp/fluxdm.sock https://example.org/file.iso /tmp/file.iso
"""
import socket
import struct
import sys

MAGIC, VERSION = 0x464C5558, 1
CMD_START = 0x01
RSP_OK, RSP_ERROR, RSP_PROGRESS, RSP_COMPLETE = 0x10, 0x11, 0x12, 0x13
FLAG_EVENT = 0x0001

HEADER = struct.Struct("<IBBHII")           # magic, version, type, flags, id, payload_len: 16 bytes
START = struct.Struct("<IHHQB32s7x")        # id (ignored), segments, flags, limit, cksum type, cksum: 56 bytes
PROGRESS = struct.Struct("<IQQIIBB2x")      # 32 bytes
ERROR = struct.Struct("<B3x124s")           # 128 bytes


def frame(msg_type, dl_id, payload=b""):
    return HEADER.pack(MAGIC, VERSION, msg_type, 0, dl_id, len(payload)) + payload


def read_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("the core closed the connection")
        buf += chunk
    return bytes(buf)


def read_frame(sock):
    magic, version, msg_type, flags, dl_id, length = HEADER.unpack(read_exact(sock, HEADER.size))
    if magic != MAGIC or version != VERSION:
        raise ValueError("not a FluxDM protocol v1 frame")
    return msg_type, flags, dl_id, read_exact(sock, length)


def start_payload(url, path, segments=8, limit=0):
    u, p = url.encode(), path.encode()
    return (START.pack(0, segments, 0, limit, 0, b"")
            + struct.pack("<H", len(u)) + u
            + struct.pack("<H", len(p)) + p)


def main(sock_path, url, path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(sock_path)
        sock.sendall(frame(CMD_START, 1, start_payload(url, path)))
        while True:
            msg_type, flags, _, payload = read_frame(sock)
            if msg_type == RSP_OK and not flags & FLAG_EVENT:
                print("accepted")
            elif msg_type == RSP_PROGRESS:
                _, total, done, bps, _, segments, _ = PROGRESS.unpack(payload)
                print(f"{done:>12,} / {total:,} bytes  {bps / 1e6:6.1f} MB/s  {segments} segments")
            elif msg_type == RSP_COMPLETE:
                print("complete")
                return 0
            elif msg_type == RSP_ERROR:
                code, text = ERROR.unpack(payload)
                message = text.split(b"\0", 1)[0].decode(errors="replace")
                print(f"error {code}: {message}")
                return 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
```

Run a core in one terminal and the client in another:

```bash
make -C core && core/fluxdm-core --sock /tmp/fluxdm.sock
python3 ipc_client.py /tmp/fluxdm.sock https://example.org/file.iso /tmp/file.iso
```

For reference, `START` for download 7 (`http://x.org/f` to `/tmp/f`, 4 segments, no limit, no checksum)
is 96 bytes on the wire: a 16-byte header with `payload_len = 80`, the 56-byte fixed part, then the
two strings.

```
0000  58 55 4c 46 01 01 00 00 07 00 00 00 50 00 00 00   XULF........P...
0010  00 00 00 00 04 00 00 00 00 00 00 00 00 00 00 00   ................
0020  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0030  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0040  00 00 00 00 00 00 00 00 0e 00 68 74 74 70 3a 2f   ..........http:/
0050  2f 78 2e 6f 72 67 2f 66 06 00 2f 74 6d 70 2f 66   /x.org/f../tmp/f
```

## Compatibility

The `version` byte is checked on every frame; a client that speaks another version is disconnected.
Version 1 is stable. Anything that changes a layout above, adds a mandatory field, or changes the
meaning of an existing one is a new protocol version and goes through the same two-sided change: the
headers in `ipc/`, the core's encoder and decoder, the daemon's `CoreIPCClient`, and the tests that
pin each of them.
