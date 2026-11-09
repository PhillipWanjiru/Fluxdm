#!/usr/bin/env python3
"""FluxDM - tests/mutation_capture.py

Checks the checks. tests/capture_model_check.py can only protect the design if it fails when a formula is wrong,
so this breaks one formula at a time in a private copy of it and requires the check that pins that formula to
fail. A mutation that survives means a check that cannot tell right from wrong.

    python3 tests/mutation_capture.py        about ten seconds; exit status 1 if any mutation survives
"""
import pathlib
import re
import subprocess
import sys
import tempfile

TARGET = pathlib.Path(__file__).resolve().parent / "capture_model_check.py"

# (check that must fail, what was broken, text in the file, its replacement). The text must occur exactly once.
MUTATIONS = [
    ("C01", "threshold with the costs swapped",
     "    return c_fp / (c_fp + c_fn)\n", "    return c_fn / (c_fp + c_fn)\n"),
    ("C02", "log-odds without the prior",
     "    score = math.log(prior / (1.0 - prior))\n", "    score = 0.0\n"),
    ("C03", "entropy over the wrong logarithm",
     "math.log2(len(values)) if len(values) > 1", "math.log(len(values)) if len(values) > 1"),
    ("C04", "tiling that tolerates a one-byte hole",
     "all(r[i][1] + 1 == r[i + 1][0] for i in range(len(r) - 1))", "all(r[i][1] == r[i + 1][0] for i in range(len(r) - 1))"),
    ("C05", "union that leaves touching intervals apart",
     "        if merged and a <= merged[-1][1] + 1:\n", "        if merged and a <= merged[-1][1]:\n"),
    ("C05", "requests to fill a gap rounded down",
     "    return sum(-(-(b - a + 1) // w) for a, b in gaps)\n", "    return sum((b - a + 1) // w for a, b in gaps)\n"),
    ("C06", "byte range that skips a byte",
     "            offset = next_byte\n", "            offset = next_byte + 1\n"),
    ("C07", "IV in little-endian order",
     '.to_bytes(16, "big")', '.to_bytes(16, "little")'),
    ("C08", "target duration that rounds up",
     "    return max(math.floor(d + 0.5) for d in durations)\n", "    return max(math.ceil(d) for d in durations)\n"),
    ("C09", "peak taken as the lowest window rate",
     "    return max(rates)\n", "    return min(rates)\n"),
    ("C09", "windows that may last only one target duration",
     "            if duration > 1.5 * target:\n", "            if duration > 1.0 * target:\n"),
    ("C10", "timeline that repeats one time too few",
     "            count = r + 1\n", "            count = r\n"),
    ("C11", "skew bound with the roles of the tracks swapped",
     "        need(-dv < s_v - s_a < da, ", "        need(-da < s_v - s_a < dv, "),
    ("C12", "chunk size without the (1 - eta) term",
     "    return eta * rate_per_connection * request_time / (1.0 - eta)\n", "    return eta * rate_per_connection * request_time\n"),
    ("C13", "break-even with the rates added",
     "    return extra_setup * r_one * r_many / (r_many - r_one)\n", "    return extra_setup * r_one * r_many / (r_many + r_one)\n"),
    ("C13", "rate that ignores the server's cap",
     "    return min(link, n * per_connection, server)\n", "    return min(link, n * per_connection)\n"),
    ("C14", "list scheduling onto the busiest connection",
     "        loads[loads.index(min(loads))] += job\n", "        loads[loads.index(max(loads))] += job\n"),
    ("C15", "assembly rule that picks the dearer way",
     '    return ("probe" if probe_cost < copy_cost else "concatenate")', '    return ("probe" if probe_cost > copy_cost else "concatenate")'),
    ("C16", "window that lets one segment too many start",
     "            if next_index >= head + window:\n", "            if next_index >= head + window + 1:\n"),
    ("C17", "refreshes that forget the first URL is free",
     "    return max(0, math.ceil(transfer_time / ttl - 1e-12) - 1)\n", "    return max(0, math.ceil(transfer_time / ttl - 1e-12))\n"),
    ("C18", "routes ordered by cost alone",
     "        ratio_order = sorted(range(k), key=lambda i: costs[i] / successes[i])\n", "        ratio_order = sorted(range(k), key=lambda i: costs[i])\n"),
    ("C19", "estimate without the prior",
     "(s + 1) / (s + f + 2)) < 1e-7", "(s + 1) / (s + f + 1)) < 1e-7"),
    ("C20", "half-open pieces taken as closed",
     "union_closed([(a, b - 1) for a, b in appended])", "union_closed([(a, b) for a, b in appended])"),
]


def run(source):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "capture_model_check.py"
        path.write_text(source, encoding="utf-8")
        done = subprocess.run([sys.executable, "-I", str(path)], capture_output=True, text=True, timeout=300)
    return done.returncode, re.findall(r"✘\s+(C\d+)", done.stdout)


def main():
    source = TARGET.read_text(encoding="utf-8")
    code, failed = run(source)
    if code != 0:
        print("the unmodified check does not pass; fix that first:", failed)
        return 2
    survived = 0
    for ident, what, old, new in MUTATIONS:
        if source.count(old) != 1:
            print(f"  ?  {ident}  {what}: the text to break occurs {source.count(old)} times; update this table")
            survived += 1
            continue
        code, failed = run(source.replace(old, new))
        caught = code != 0 and ident in failed
        print(f"  {'caught ' if caught else 'SURVIVED'}  {ident}  {what}" + ("" if caught else f"  (failed: {failed})"))
        survived += not caught
    print(f"\n  {len(MUTATIONS) - survived} of {len(MUTATIONS)} mutations caught")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
