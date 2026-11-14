#!/usr/bin/env python3
"""FluxDM - native messaging host.

The browser starts this program when the extension calls chrome.runtime.sendNativeMessage("com.fluxdm.host", ...)
and talks to it over stdin and stdout in Chrome's framing: a 4-byte message length in native byte order, then that
many bytes of UTF-8 JSON. Every message is answered with one message.

    {"type": "ping"}                       ->  {"status": "ok", "version": "...", "handoff": false}
    {"type": "download", "url": "..."}     ->  {"status": "error", "error": "..."}
    anything else                          ->  {"status": "error", "error": "..."}

The extension cancels the browser's own download only when the reply is {"status": "accepted"}. This host does not
send that yet: passing a download on to the daemon (with the API token) is not built (docs/ROADMAP.md, "Browser-
extension hand-off"), and until it is, the browser has to keep the download rather than lose it to a host that
says "ok" to everything.

The input is not trusted to be well formed: frames are limited in size, bad JSON and messages of the wrong shape
are answered with an error and do not end the host, and only a closed pipe or an oversized frame does.
"""
import json
import struct
import sys
from urllib.parse import urlsplit

VERSION = "0.1.0"
MAX_MESSAGE = 64 * 1024      # bytes accepted from the browser; an offer is well under 2 KB
MAX_URL = 8192               # the extension sends no more either


class FrameError(Exception):
    """The browser sent a frame this host will not read."""


def read_frame(stream):
    """The payload of the next frame, or None when the browser has closed the pipe (also in the middle of a frame)."""
    header = stream.read(4)
    if len(header) < 4:
        return None
    (length,) = struct.unpack("=I", header)
    if length > MAX_MESSAGE:
        raise FrameError("a message of %d bytes is larger than the %d bytes accepted" % (length, MAX_MESSAGE))
    body = stream.read(length)
    return body if len(body) == length else None


def write_message(stream, message):
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    stream.write(struct.pack("=I", len(payload)) + payload)
    stream.flush()


def error(text):
    return {"status": "error", "error": text}


def web_address_problem(value, name):
    """Why `value` is not the address of something that can be downloaded, or None if it is."""
    if not isinstance(value, str) or not value:
        return "%s is missing" % name
    if len(value) > MAX_URL:
        return "%s is too long" % name
    if any(c <= " " or "\x7f" <= c <= "\x9f" for c in value):
        return "%s has a blank or a control character" % name
    try:
        parts = urlsplit(value)
    except ValueError:
        return "%s is not an address" % name
    if parts.scheme not in ("http", "https") or not parts.netloc:
        return "%s is not an http or https address" % name
    return None


def handle(message):
    """The reply to one decoded message."""
    if not isinstance(message, dict):
        return error("a message must be a JSON object")
    kind = message.get("type")
    if kind == "ping":
        return {"status": "ok", "version": VERSION, "handoff": False}
    if kind == "download":
        for name in ("url", "final_url", "referrer"):
            if name == "url" or name in message:
                problem = web_address_problem(message.get(name), name)
                if problem:
                    return error(problem)
        return error("this host does not pass downloads to FluxDM yet, so the browser keeps the download")
    return error("unknown message type")


def main(stdin=None, stdout=None):
    """Answer messages until the browser closes the pipe. Returns the exit status."""
    stdin = stdin or sys.stdin.buffer
    stdout = stdout or sys.stdout.buffer
    while True:
        try:
            frame = read_frame(stdin)
        except FrameError as problem:
            write_message(stdout, error(str(problem)))
            return 1                     # the stream cannot be trusted to be in step any more
        if frame is None:
            return 0
        try:
            message = json.loads(frame.decode("utf-8"))
        except (UnicodeDecodeError, ValueError, RecursionError):
            write_message(stdout, error("the message is not valid JSON"))
            continue
        write_message(stdout, handle(message))


if __name__ == "__main__":
    sys.exit(main())
