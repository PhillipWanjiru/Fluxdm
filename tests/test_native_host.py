#!/usr/bin/env python3
"""FluxDM - tests/test_native_host.py

The browser extension's native messaging host, run as the browser runs it: a real process, Chrome's framing
(a 4-byte length in native byte order, then JSON) on stdin and stdout.

    python3 tests/test_native_host.py            (Python 3.10 or newer, standard library only)

The rule they pin down together with tests/test_extension.js: the extension cancels the browser's download only
when the host answers {"status": "accepted"}, and this host cannot pass a download on yet, so it must never say
so. It used to answer {"status": "ok"} to everything, exit on the first message that was falsy (`{}`, `[]`, `0`),
and read whatever length it was told to.
"""
import importlib.util
import json
import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path

HOST = Path(__file__).resolve().parent.parent / "extension" / "native" / "native_host.py"

spec = importlib.util.spec_from_file_location("native_host", HOST)
native_host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native_host)


def frame(message) -> bytes:
    body = message if isinstance(message, bytes) else json.dumps(message).encode()
    return struct.pack("=I", len(body)) + body


def run_host(*chunks: bytes, timeout: float = 20):
    """Start the host, write the chunks to its stdin, close it, and return (exit status, replies, stderr)."""
    done = subprocess.run([sys.executable, "-I", str(HOST)], input=b"".join(chunks), capture_output=True, timeout=timeout)
    replies, out, i = [], done.stdout, 0
    while i < len(out):
        (length,) = struct.unpack("=I", out[i:i + 4])
        replies.append(json.loads(out[i + 4:i + 4 + length]))
        i += 4 + length
        assert i <= len(out), "a reply was cut short"
    return done.returncode, replies, done.stderr


class Replies(unittest.TestCase):
    def test_ping(self):
        rc, replies, err = run_host(frame({"type": "ping"}))
        self.assertEqual((rc, err), (0, b""))
        self.assertEqual(replies, [{"status": "ok", "version": native_host.VERSION, "handoff": False}])

    def test_every_message_gets_exactly_one_reply_in_order(self):
        rc, replies, _ = run_host(frame({"type": "ping"}), frame({"type": "nope"}), frame({"type": "ping"}))
        self.assertEqual(rc, 0)
        self.assertEqual([r["status"] for r in replies], ["ok", "error", "ok"])

    def test_it_never_accepts_a_download_while_it_cannot_pass_it_on(self):
        offers = [
            {"type": "download", "url": "https://files.example/a.iso"},
            {"type": "download", "url": "http://h/x?y=1#z", "final_url": "https://cdn.example/x", "referrer": "https://h/p",
             "mime": "application/zip", "size": 5},
            {"type": "download", "url": "https://files.example/a.iso", "status": "accepted"},
            {"type": "download", "url": "https://files.example/a.iso", "accepted": True},
            {"type": "accepted"}, {"status": "accepted"}, {"type": "ping", "status": "accepted"},
        ]
        rc, replies, _ = run_host(*[frame(o) for o in offers])
        self.assertEqual(rc, 0)
        self.assertEqual(len(replies), len(offers))
        for offer, reply in zip(offers, replies):
            self.assertNotEqual(reply.get("status"), "accepted", offer)
        for reply in replies[:4]:
            self.assertEqual(reply["status"], "error")
        self.assertIn("browser keeps the download", replies[0]["error"])

    def test_the_address_is_checked(self):
        bad = {
            "missing": {"type": "download"},
            "not a string": {"type": "download", "url": 5},
            "a list": {"type": "download", "url": ["https://h/x"]},
            "empty": {"type": "download", "url": ""},
            "javascript": {"type": "download", "url": "javascript:alert(1)"},
            "file": {"type": "download", "url": "file:///etc/passwd"},
            "ftp": {"type": "download", "url": "ftp://h/x"},
            "data": {"type": "download", "url": "data:text/plain,hi"},
            "blob": {"type": "download", "url": "blob:https://h/1"},
            "no host": {"type": "download", "url": "http://"},
            "no slashes": {"type": "download", "url": "https:h/x"},
            "blank": {"type": "download", "url": "https://h/a b"},
            "newline": {"type": "download", "url": "https://h/a\nb"},
            "control": {"type": "download", "url": "https://h/a\x00b"},
            "delete": {"type": "download", "url": "https://h/a\x7fb"},
            "too long": {"type": "download", "url": "https://h/" + "a" * native_host.MAX_URL},
            "bad final address": {"type": "download", "url": "https://h/x", "final_url": "ftp://h/x"},
            "bad referrer": {"type": "download", "url": "https://h/x", "referrer": "javascript:1"},
            "referrer is not a string": {"type": "download", "url": "https://h/x", "referrer": 7},
        }
        rc, replies, _ = run_host(*[frame(m) for m in bad.values()])
        self.assertEqual(rc, 0)
        by_case = dict(zip(bad, replies))
        for why, reply in by_case.items():
            self.assertEqual(reply["status"], "error", why)
            self.assertNotIn("does not pass downloads", reply["error"], f"{why}: refused for being malformed, not for the missing hand-off")
        self.assertIn("url", by_case["missing"]["error"])
        self.assertIn("final_url", by_case["bad final address"]["error"])
        self.assertIn("referrer", by_case["bad referrer"]["error"])
        self.assertIn("referrer", by_case["referrer is not a string"]["error"])

    def test_unknown_and_missing_types(self):
        rc, replies, _ = run_host(*[frame(m) for m in ({"type": "launch"}, {"type": 5}, {"type": None}, {"url": "https://h/x"})])
        self.assertEqual(rc, 0)
        self.assertEqual([r["status"] for r in replies], ["error"] * 4)

    def test_replies_are_framed_in_native_byte_order(self):
        done = subprocess.run([sys.executable, "-I", str(HOST)], input=frame({"type": "ping"}), capture_output=True, timeout=20)
        (length,) = struct.unpack("=I", done.stdout[:4])
        self.assertEqual(length, len(done.stdout) - 4)
        self.assertEqual(done.stdout[:4], len(done.stdout[4:]).to_bytes(4, sys.byteorder))
        self.assertTrue(done.stdout[4:].isascii(), "a reply is plain ASCII JSON")


class BadInput(unittest.TestCase):
    def test_messages_that_are_falsy_do_not_end_the_host(self):
        # `if not msg: break` ended the host at {}, [], 0, "" and null, so the browser saw it vanish
        falsy = [{}, [], 0, "", None, False]
        rc, replies, _ = run_host(*[frame(m) for m in falsy], frame({"type": "ping"}))
        self.assertEqual(rc, 0)
        self.assertEqual(len(replies), len(falsy) + 1)
        self.assertEqual([r["status"] for r in replies], ["error"] * len(falsy) + ["ok"])

    def test_messages_that_are_not_objects(self):
        rc, replies, _ = run_host(*[frame(m) for m in ([1, 2], "ping", 3.5, True, [{"type": "ping"}])], frame({"type": "ping"}))
        self.assertEqual(rc, 0)
        self.assertEqual([r["status"] for r in replies], ["error"] * 5 + ["ok"])

    def test_bad_json_is_answered_and_the_host_carries_on(self):
        rc, replies, err = run_host(frame(b"{not json"), frame(b""), frame(b"{"), frame(b"\xff\xfe"), frame({"type": "ping"}))
        self.assertEqual((rc, err), (0, b""))
        self.assertEqual([r["status"] for r in replies], ["error"] * 4 + ["ok"])
        self.assertIn("JSON", replies[0]["error"])

    def test_very_deep_nesting_is_an_error_not_a_crash(self):
        deep = b"[" * 30000 + b"]" * 30000
        self.assertLess(len(deep), native_host.MAX_MESSAGE)
        rc, replies, err = run_host(frame(deep), frame({"type": "ping"}))
        self.assertEqual((rc, err), (0, b""))
        self.assertEqual([r["status"] for r in replies], ["error", "ok"])

    def test_non_ascii_text_is_fine(self):
        rc, replies, _ = run_host(frame({"type": "download", "url": "https://h/café"}), frame({"type": "ping", "note": "€ \U0001f600"}))
        self.assertEqual(rc, 0)
        self.assertEqual(len(replies), 2)
        self.assertEqual(replies[1]["status"], "ok")

    def test_the_size_limit_is_exact(self):
        pad = native_host.MAX_MESSAGE - len(b'{"type":"ping","pad":""}')
        at_limit = b'{"type":"ping","pad":"' + b"x" * pad + b'"}'
        self.assertEqual(len(at_limit), native_host.MAX_MESSAGE)
        rc, replies, _ = run_host(frame(at_limit), frame({"type": "ping"}))
        self.assertEqual((rc, [r["status"] for r in replies]), (0, ["ok", "ok"]))

    def test_an_oversized_frame_is_refused_without_reading_it(self):
        # 2 GiB announced, three bytes sent: a host that believes the length waits for, or allocates, the rest
        for announced in (native_host.MAX_MESSAGE + 1, 2 ** 31, 2 ** 32 - 1):
            rc, replies, _ = run_host(struct.pack("=I", announced) + b"abc", timeout=20)
            self.assertEqual(rc, 1, announced)
            self.assertEqual([r["status"] for r in replies], ["error"], announced)
            self.assertIn("larger", replies[0]["error"])

    def test_the_end_of_the_input_is_a_clean_exit(self):
        self.assertEqual(run_host(), (0, [], b""))
        self.assertEqual(run_host(b"\x01\x00"), (0, [], b""), "half a length header")
        self.assertEqual(run_host(struct.pack("=I", 10) + b"abc"), (0, [], b""), "a frame cut short")
        rc, replies, _ = run_host(frame({"type": "ping"}), struct.pack("=I", 10) + b"abc")
        self.assertEqual((rc, [r["status"] for r in replies]), (0, ["ok"]), "what was complete is answered")


class Program(unittest.TestCase):
    def test_importing_the_module_does_not_start_the_loop(self):
        self.assertTrue(callable(native_host.main) and callable(native_host.handle))   # the import above returned

    def test_it_is_a_program_the_browser_can_start(self):
        self.assertTrue(os.access(HOST, os.X_OK), "the browser starts the file named in the host manifest, which must be executable")
        self.assertEqual(HOST.read_bytes().split(b"\n", 1)[0], b"#!/usr/bin/env python3")

    def test_it_writes_nothing_but_replies_to_stdout(self):
        _, _, err = run_host(frame({"type": "ping"}), frame(b"junk"), frame({"type": "download", "url": "https://h/x"}))
        self.assertEqual(err, b"", "nothing on stderr for ordinary input")


if __name__ == "__main__":
    unittest.main(verbosity=2)
