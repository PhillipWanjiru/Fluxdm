#!/usr/bin/env python3
"""FluxDM - tests/capture_model_check.py

Runs the formulas of docs/VIDEO_CAPTURE.md and checks each one against brute force, exhaustive search or
simulation, so that the design's mathematics is something the repository can fail on, and so that the code
written from it later (in the extension, the daemon and the core) has an oracle to be compared with.

  - Every number the document quotes as a worked example (E1 .. E9) is asserted here.
  - Every check carries the id (C01 ..) and the section of the document it pins.
  - Standard library only. Deterministic: each check seeds its own generator from its id.

    python3 tests/capture_model_check.py             all checks (a few seconds)
    python3 tests/capture_model_check.py -k sched    the checks whose id or title contains "sched"
    python3 tests/capture_model_check.py --list      the checks, and the section each one pins
"""
import argparse
import itertools
import math
import random
import re
import sys
import zlib
from collections import Counter
from heapq import heappop, heappush

SEED = 20261109
CHECKS = []


class CheckFailed(AssertionError):
    pass


def need(condition, message):
    if not condition:
        raise CheckFailed(message)


def close(a, b, rel=1e-9, floor=1e-12):
    return abs(a - b) <= max(rel * max(abs(a), abs(b)), floor)


def check(ident, section, title):
    def register(fn):
        CHECKS.append((ident, section, title, fn))
        return fn
    return register


# --- 4. Detection is a decision under cost -----------------------------------------------------------------

def offer_threshold(c_fp, c_fn):
    """Offer a candidate when P(media | evidence) is above this (section 4.2)."""
    return c_fp / (c_fp + c_fn)


def posterior_direct(prior, p_if_media, p_if_other, x):
    """P(media | x) by Bayes' rule, the features taken as independent given the class (naive Bayes)."""
    media, other = prior, 1.0 - prior
    for a, b, seen in zip(p_if_media, p_if_other, x):
        media *= a if seen else 1.0 - a
        other *= b if seen else 1.0 - b
    return media / (media + other)


def posterior_log_odds(prior, p_if_media, p_if_other, x):
    """The same posterior as prior log-odds plus one log likelihood ratio per feature (section 4.3)."""
    score = math.log(prior / (1.0 - prior))
    for a, b, seen in zip(p_if_media, p_if_other, x):
        score += math.log((a if seen else 1.0 - a) / (b if seen else 1.0 - b))
    return 1.0 / (1.0 + math.exp(-score))


@check("C01", "4.2", "offer a candidate when P(media) > c_fp / (c_fp + c_fn)")
def c01(rng):
    for _ in range(5000):
        c_fp, c_fn = rng.uniform(0.05, 20), rng.uniform(0.05, 20)
        p = rng.uniform(1e-6, 1 - 1e-6)
        offer_cost = (1 - p) * c_fp   # not media with probability 1-p: the user gets a useless entry
        skip_cost = p * c_fn          # media with probability p: the video is missed
        tau = offer_threshold(c_fp, c_fn)
        need((offer_cost < skip_cost) == (p > tau), f"c_fp={c_fp} c_fn={c_fn} p={p}")
        need((math.log(p / (1 - p)) > math.log(c_fp / c_fn)) == (p > tau), "the log-odds form disagrees")
    need(close(offer_threshold(1, 5), 1 / 6), "E1: a missed video five times as bad as a useless entry gives 1/6")


@check("C02", "4.3", "the posterior is the prior odds times one likelihood ratio per feature")
def c02(rng):
    for _ in range(500):
        k = rng.randint(1, 8)
        prior = rng.uniform(0.001, 0.5)
        a = [rng.uniform(0.02, 0.98) for _ in range(k)]
        b = [rng.uniform(0.02, 0.98) for _ in range(k)]
        x = [rng.random() < 0.5 for _ in range(k)]
        need(close(posterior_direct(prior, a, b, x), posterior_log_odds(prior, a, b, x)), "the two forms differ")
    # E1: a prior of 2 %; the likelihood ratios are examples, not measurements
    odds = 0.02 / 0.98
    tau = offer_threshold(1, 5)
    for ratio, expected, offered in ((30, 0.380, True), (3, 0.0577, False), (400 * 8, 0.985, True)):
        p = odds * ratio / (1 + odds * ratio)
        need(abs(p - expected) < 5e-4, f"E1: ratio {ratio} gives {p:.4f}, the document says {expected}")
        need((p > tau) == offered, f"E1: ratio {ratio}: offered should be {offered}")


# --- 5. Which requests are one stream ----------------------------------------------------------------------

def entropy_bits(values):
    n = len(values)
    return -sum(c / n * math.log2(c / n) for c in Counter(values).values())


def variability(values):
    """H / log2(m): 0 when every request carries the same value, 1 when every request carries a different one."""
    return entropy_bits(values) / math.log2(len(values)) if len(values) > 1 else 0.0


def tiles(ranges):
    """True when the closed ranges, sorted, follow one another with no gap and no overlap (repeats are ignored)."""
    r = sorted(set(ranges))
    return all(a <= b for a, b in r) and all(r[i][1] + 1 == r[i + 1][0] for i in range(len(r) - 1))


def classify_parameter(values):
    """'constant', 'position' (byte ranges a-b that tile one span, or consecutive numbers) or 'other'."""
    if len(set(values)) == 1:
        return "constant"
    if all(re.fullmatch(r"\d+-\d+", v) for v in values):
        ranges = [tuple(int(part) for part in v.split("-")) for v in values]
        return "position" if tiles(ranges) else "other"
    if all(re.fullmatch(r"\d+", v) for v in values):
        numbers = sorted({int(v) for v in values})
        return "position" if numbers[-1] - numbers[0] + 1 == len(numbers) else "other"
    return "other"


@check("C03", "5.2", "variability of a parameter: 0 when constant, 1 when every value differs")
def c03(rng):
    need(variability(["a"] * 7) == 0.0, "a constant parameter must score 0")
    need(close(variability([str(i) for i in range(9)]), 1.0), "all-different must score 1")
    need(close(variability(["x"] * 4 + ["y"] * 4), 1 / 3), "half and half over 8 requests: 1 bit of 3")
    for _ in range(500):
        m = rng.randint(2, 40)
        values = [rng.randint(0, 6) for _ in range(m)]
        v = variability(values)
        need(-1e-12 <= v <= 1 + 1e-12, f"{values} -> {v}")
        need((len(set(values)) == m) == close(v, 1.0), f"{values}: 1 exactly when all differ")
    for _ in range(500):                  # d values, each used q times, in m = d * q requests
        d, q = rng.randint(1, 30), rng.randint(1, 30)
        values = [str(i) for i in range(d) for _ in range(q)]
        need((variability(values) <= 0.5 + 1e-12) == (d <= q), f"d={d} q={q}: v <= 1/2 exactly when d <= sqrt(m)")


@check("C04", "5.2", "a parameter that tiles one span is a position; a gap, an overlap or a timestamp is not")
def c04(rng):
    def text(ranges):
        return [f"{a}-{b}" for a, b in ranges]

    for _ in range(300):
        pieces, a = [], 0
        for _ in range(rng.randint(3, 12)):
            n = rng.randint(1, 5000)
            pieces.append((a, a + n - 1))
            a += n
        order = pieces[:]
        rng.shuffle(order)
        order.append(rng.choice(pieces))                       # the player asked for one piece twice
        need(classify_parameter(text(order)) == "position", "a shuffled tiling with a repeat is a position")
        hole = pieces[:]
        del hole[rng.randrange(1, len(hole) - 1)]              # drop an inner piece: a hole in the span
        need(classify_parameter(text(hole)) == "other", "a hole was taken for a position")
        overlap = pieces[:] + [(pieces[1][0], pieces[1][1] + 1)]   # same start, one byte longer
        need(classify_parameter(text(overlap)) == "other", "an overlap was taken for a position")
    need(classify_parameter(["17", "18", "19", "20"]) == "position", "consecutive sequence numbers")
    need(classify_parameter(["17", "19", "20"]) == "other", "a missing sequence number")
    need(classify_parameter(["1699999991", "1699999994", "1700000007"]) == "other", "timestamps are noise")
    need(classify_parameter(["tok"] * 5) == "constant", "a token is constant")


# --- 5.3 Range algebra --------------------------------------------------------------------------------------

def union_closed(intervals):
    """Merge closed integer intervals [a, b]; intervals that touch (b + 1 == next a) are merged too."""
    merged = []
    for a, b in sorted(intervals):
        if merged and a <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return [(a, b) for a, b in merged]


def gaps_of(covered, size):
    """The parts of [0, size - 1] that `covered` (merged, sorted, inside the span) leaves out."""
    out, pos = [], 0
    for a, b in covered:
        if a > pos:
            out.append((pos, a - 1))
        pos = max(pos, b + 1)
    if pos < size:
        out.append((pos, size - 1))
    return out


def requests_to_fill(gaps, w):
    """Requests of at most w bytes, none over bytes already held: the sum of ceil(gap / w)."""
    return sum(-(-(b - a + 1) // w) for a, b in gaps)


def content_range(value):
    """'bytes a-b/total' -> (a, b, total); total is None for '*'. 'bytes */total' -> (None, None, total)."""
    m = re.fullmatch(r"bytes (?:(\d+)-(\d+)|\*)/(\d+|\*)", value.strip())
    need(m is not None, f"not a Content-Range: {value!r}")
    first = int(m.group(1)) if m.group(1) is not None else None
    last = int(m.group(2)) if m.group(2) is not None else None
    return first, last, None if m.group(3) == "*" else int(m.group(3))


@check("C05", "5.3", "union, gaps and the cost of filling them agree with a bitmap")
def c05(rng):
    for _ in range(1500):
        size = rng.randint(1, 200)
        raw = []
        for _ in range(rng.randint(0, 12)):
            a = rng.randrange(size)
            raw.append((a, rng.randrange(a, size)))
        bitmap = [False] * size
        for a, b in raw:
            for i in range(a, b + 1):
                bitmap[i] = True
        merged = union_closed(raw)
        again = [False] * size
        for a, b in merged:
            for i in range(a, b + 1):
                again[i] = True
        need(bitmap == again, "the union covers different bytes")
        need(all(merged[i][1] + 1 < merged[i + 1][0] for i in range(len(merged) - 1)), "merged intervals touch")
        need(union_closed(merged + raw) == merged, "the union is not idempotent")
        holes = gaps_of(merged, size)
        missing = [i for i, held in enumerate(bitmap) if not held]
        need([i for a, b in holes for i in range(a, b + 1)] == missing, "the gaps are not the missing bytes")
        w = rng.randint(1, 40)
        chunks = 0
        for a, b in holes:                      # the obvious way: walk each hole in steps of w
            pos = a
            while pos <= b:
                pos += w
                chunks += 1
        need(chunks == requests_to_fill(holes, w), "ceil(gap / w) does not count the requests")
    need(content_range("bytes 200-1023/146515") == (200, 1023, 146515), "Content-Range with a total")
    need(content_range("bytes 0-99/*") == (0, 99, None), "Content-Range with an unknown total")
    need(content_range("bytes */1234") == (None, None, 1234), "an unsatisfied range names the total")
    need(requests_to_fill([(100, 199)], 64) == 2, "a hole of 100 bytes in requests of 64 bytes")


# --- 6. Manifests -----------------------------------------------------------------------------------------

def resolve_byteranges(entries):
    """RFC 8216 4.3.2.2. entries: (length, offset or None) in playlist order, for one resource."""
    out, next_byte = [], None
    for length, offset in entries:
        if offset is None:
            if next_byte is None:
                raise ValueError("a range without an offset needs a previous range of the same resource")
            offset = next_byte
        out.append((offset, offset + length - 1))
        next_byte = offset + length
    return out


@check("C06", "6.1", "HLS byte ranges without an offset continue the previous range and tile the resource")
def c06(rng):
    for _ in range(500):
        start = rng.choice([0, 0, 188])
        lengths = [rng.randint(1, 5000) for _ in range(rng.randint(1, 30))]
        expected, pos = [], start
        for n in lengths:
            expected.append((pos, pos + n - 1))
            pos += n
        entries = [(n, first if i == 0 or rng.random() < 0.3 else None)
                   for i, (n, (first, _)) in enumerate(zip(lengths, expected))]
        need(resolve_byteranges(entries) == expected, "offsets were not resolved to the tiling")
        need(union_closed(resolve_byteranges(entries)) == [(start, pos - 1)], "the ranges do not tile one span")
    try:
        resolve_byteranges([(100, None)])
    except ValueError:
        pass
    else:
        need(False, "a first range without an offset must be refused")


def hls_default_iv(media_sequence, index):
    """RFC 8216 5.2: with no IV attribute the segment's Media Sequence Number, big-endian, in 16 octets."""
    return (media_sequence + index).to_bytes(16, "big")


@check("C07", "6.1", "the default AES-128 IV is the media sequence number as 16 big-endian octets")
def c07(rng):
    def by_hand(n):
        out = bytearray(16)
        for i in range(15, -1, -1):
            out[i] = n & 0xFF
            n >>= 8
        return bytes(out)

    need(hls_default_iv(7, 3) == bytes(15) + b"\x0a", "sequence 7, third segment after it")
    for _ in range(1000):
        seq, i = rng.randrange(2 ** 64), rng.randrange(100000)
        iv = hls_default_iv(seq, i)
        need(len(iv) == 16 and iv == by_hand(seq + i), f"{seq}+{i}")
        need(iv[:7] == bytes(7), "a 64-bit sequence number leaves the high octets zero")


def min_target_duration(durations):
    """The smallest integer EXT-X-TARGETDURATION that RFC 8216 4.3.3.1 allows: every EXTINF, rounded, <= it."""
    return max(math.floor(d + 0.5) for d in durations)


@check("C08", "6.1", "the smallest valid EXT-X-TARGETDURATION is the largest rounded EXTINF")
def c08(rng):
    for _ in range(1000):
        durations = [rng.uniform(0.5, 12) for _ in range(rng.randint(1, 40))]
        target = min_target_duration(durations)
        need(all(math.floor(d + 0.5) <= target for d in durations), "a segment is longer than the target")
        need(any(math.floor(d + 0.5) > target - 1 for d in durations), "a target one smaller must be invalid")
        need(all(d < target + 0.5 for d in durations), "no segment may reach the target plus half a second")


def peak_segment_bit_rate(segments, target):
    """RFC 8216 4.1: the largest bit rate of any contiguous set of segments whose total duration is between 0.5 and
    1.5 times the target duration; the bit rate of a set is its total size over its total duration.
    segments: (bytes, seconds). Returns bits per second, or 0.0 when no set qualifies."""
    rates = [0.0]
    for first in range(len(segments)):
        size = duration = 0.0
        for s, d in segments[first:]:
            size += s
            duration += d
            if duration > 1.5 * target:
                break
            if duration >= 0.5 * target:
                rates.append(8 * size / duration)
    return max(rates)


@check("C09", "6.1", "BANDWIDTH (the windowed peak) bounds the size of a variant; AVERAGE-BANDWIDTH gives it back")
def c09(rng):
    for _ in range(1000):
        target = rng.randint(4, 12)
        # every segment lasts between half and one and a half target durations, so each is a window by itself
        segments = [(rng.randint(10_000, 4_000_000), rng.uniform(0.5 * target + 0.01, target + 0.49))
                    for _ in range(rng.randint(1, 60))]
        size = sum(s for s, _ in segments)
        duration = sum(d for _, d in segments)
        peak = peak_segment_bit_rate(segments, target)
        average = 8 * size / duration
        need(peak >= max(8 * s / d for s, d in segments) * (1 - 1e-12), "a segment on its own is a window")
        need(average <= peak * (1 + 1e-12), "the average bit rate exceeds the peak")
        need(size <= peak * duration / 8 * (1 + 1e-12), "BANDWIDTH x duration / 8 is not an upper bound")
        need(close(average * duration / 8, size), "AVERAGE-BANDWIDTH x duration / 8 is not the size")
    # the caveat the document states: a last segment shorter than half a target duration can lie outside every window
    target = 10
    short_tail = [(1_300_000, 10.4), (6_125_000, 4.9)]        # 1 Mbit/s, then 10 Mbit/s for 4.9 s
    need(close(peak_segment_bit_rate(short_tail, target), 1_300_000 * 8 / 10.4), "only the first segment is a window")
    need(sum(s for s, _ in short_tail) > peak_segment_bit_rate(short_tail, target) * 15.3 / 8,
         "a segment outside every window can break the bound")
    need(close(5_000_000 * 7200 / 8, 4.5e9), "E9: two hours at 5 Mbit/s peak")
    need(close(3_200_000 * 7200 / 8, 2.88e9), "E9: two hours at 3.2 Mbit/s average")


def expand_timeline(entries, period_end, start_number=1):
    """DASH SegmentTimeline. entries: dicts with 'd', optional 't' and 'r' (r >= 0 repeats; r = -1 runs on to the
    next entry's t, or to period_end). Returns (number, start, duration) in timescale ticks."""
    out, cursor, number = [], 0, start_number
    for i, e in enumerate(entries):
        t = e.get("t", cursor)
        r = e.get("r", 0)
        if r < 0:
            until = entries[i + 1]["t"] if i + 1 < len(entries) and "t" in entries[i + 1] else period_end
            count = max(0, -(-(until - t) // e["d"]))
        else:
            count = r + 1
        for k in range(count):
            out.append((number, t + k * e["d"], e["d"]))
            number += 1
        cursor = t + count * e["d"]
    return out


@check("C10", "6.2", "SegmentTimeline and SegmentTemplate expand to the counts and times the formulas give")
def c10(rng):
    for _ in range(500):
        timescale = rng.choice([1, 1000, 90000])
        entries, cursor = [], 0
        for i in range(rng.randint(1, 6)):
            e = {"d": rng.randint(1, 10) * timescale, "r": rng.randint(0, 8)}
            if i == 0 and rng.random() < 0.5:                  # a live timeline starts well after zero
                cursor = e["t"] = rng.randint(1, 1000) * timescale
            elif i > 0 and rng.random() < 0.3:                 # a discontinuity: this entry names its own start
                cursor += rng.randint(1, 5) * timescale
                e["t"] = cursor
            entries.append(e)
            cursor += e["d"] * (e["r"] + 1)
        start_number = rng.randint(0, 100)
        segments = expand_timeline(entries, period_end=cursor, start_number=start_number)
        need(len(segments) == sum(e["r"] + 1 for e in entries), "N is the sum of (r + 1)")
        need([n for n, _, _ in segments] == list(range(start_number, start_number + len(segments))), "$Number$")
        need(sum(d for _, _, d in segments) == sum(e["d"] * (e["r"] + 1) for e in entries), "total duration")
        need(segments[-1][1] + segments[-1][2] == cursor, "the last segment ends where the timeline does")
        need(all(segments[i][1] + segments[i][2] <= segments[i + 1][1] for i in range(len(segments) - 1)),
             "segments overlap")
    # r = -1 runs to the next explicit start, or to the end of the period
    open_ended = [{"t": 0, "d": 4, "r": -1}, {"t": 40, "d": 5, "r": -1}]
    expanded = expand_timeline(open_ended, period_end=62)
    need(len([s for s in expanded if s[1] < 40]) == 10, "r=-1 up to the next t: ceil(40 / 4) segments")
    need(len([s for s in expanded if s[1] >= 40]) == 5, "r=-1 to the period end: ceil(22 / 5) segments")
    # a template with one fixed duration: N = ceil(period / duration), and the last segment is the short one
    for _ in range(300):
        d, period = rng.randint(1, 50), rng.randint(1, 5000)
        n = -(-period // d)
        starts = [k * d for k in range(n)]
        need(starts[-1] < period <= starts[-1] + d, "N = ceil(period / d)")
        need(0 < period - starts[-1] <= d, "the last segment is no longer than the others")


@check("C11", "6.3", "starting two tracks at the segments that hold T0 leaves a skew below the longer duration")
def c11(rng):
    def grid(longest, horizon, first):
        starts = [first]
        while starts[-1] < horizon:
            starts.append(starts[-1] + rng.uniform(0.2 * longest, longest))
        return starts

    for _ in range(3000):
        dv, da = rng.uniform(1, 10), rng.uniform(1, 10)
        starts_v, starts_a = grid(dv, 120, -rng.uniform(0, dv)), grid(da, 120, -rng.uniform(0, da))
        t0 = rng.uniform(0, 100)
        s_v = max(s for s in starts_v if s <= t0)
        s_a = max(s for s in starts_a if s <= t0)
        need(-dv < s_v - s_a < da, f"video start - audio start = {s_v - s_a}, Dv={dv}, Da={da}")
        need(abs(s_v - s_a) < max(dv, da), "the skew reaches the longer segment duration")


# --- 7. Scheduling the fetch ------------------------------------------------------------------------------

def efficiency(w, rate_per_connection, request_time):
    """The share of a connection's time that carries payload when each request moves w bytes (section 7.1)."""
    on_wire = w / rate_per_connection
    return on_wire / (on_wire + request_time)


def smallest_chunk(eta, rate_per_connection, request_time):
    return eta * rate_per_connection * request_time / (1.0 - eta)


@check("C12", "7.1", "the chunk size that gives a requested efficiency is eta * r * T_req / (1 - eta)")
def c12(rng):
    for _ in range(1000):
        r, t_req, eta = rng.uniform(1e5, 5e7), rng.uniform(0.005, 0.5), rng.uniform(0.5, 0.99)
        w = smallest_chunk(eta, r, t_req)
        need(close(efficiency(w, r, t_req), eta), "efficiency at the minimum chunk")
        need(efficiency(w * 0.9, r, t_req) < eta < efficiency(w * 1.1, r, t_req), "efficiency must rise with w")
    need(close(smallest_chunk(0.9, 5e6, 0.08), 3.6e6), "E2: 5 MB/s, 80 ms, 90 %: 3.6 MB")


def rate(n, link, per_connection, server=math.inf):
    return min(link, n * per_connection, server)


def total_time(size, n, base_setup, per_extra_connection, link, per_connection, server=math.inf):
    """T = T0 + (n - 1) * delta + S / R(n): the extra connections are opened one after the other (worst case)."""
    return base_setup + (n - 1) * per_extra_connection + size / rate(n, link, per_connection, server)


def break_even_size(extra_setup, r_one, r_many):
    """The size at which the n-connection download catches up with the single connection."""
    return extra_setup * r_one * r_many / (r_many - r_one)


def connections_to_reach_cap(link, per_connection, server=math.inf):
    return max(1, math.ceil(min(link, server) / per_connection - 1e-12))


def best_connections(size, base_setup, delta, link, per_connection, server=math.inf):
    n_cap = connections_to_reach_cap(link, per_connection, server)
    n_opt = math.sqrt(size / (per_connection * delta))
    candidates = {math.floor(n_opt), math.ceil(n_opt), n_cap - 1, n_cap}
    candidates = {min(max(1, n), n_cap) for n in candidates}
    return min(candidates, key=lambda n: (total_time(size, n, base_setup, delta, link, per_connection, server), n))


@check("C13", "7.2", "R(n) is concave; the break-even size and the best number of connections follow from T(n)")
def c13(rng):
    for _ in range(300):
        link = rng.uniform(1e6, 1e8)
        r_c = rng.uniform(1e5, 2e7)
        server = rng.choice([math.inf, rng.uniform(1e6, 1e8)])
        rates = [rate(n, link, r_c, server) for n in range(0, 80)]
        tol = 1e-9 * max(rates) + 1e-9
        need(all(rates[i + 1] >= rates[i] for i in range(len(rates) - 1)), "R(n) must not fall")
        need(all(rates[i + 2] - rates[i + 1] <= rates[i + 1] - rates[i] + tol for i in range(len(rates) - 2)),
             "R(n) must be concave: each extra connection adds no more than the one before")
        cap = min(link, server)
        n_cap = connections_to_reach_cap(link, r_c, server)
        if n_cap <= 78:
            need(rate(n_cap, link, r_c, server) == cap and (n_cap == 1 or rate(n_cap - 1, link, r_c, server) < cap),
                 "n* = ceil(cap / r_c) is the first n that reaches the cap")
        # break-even between one connection and n
        n = rng.randint(2, 16)
        delta = rng.uniform(0.01, 0.5)
        t0 = rng.uniform(0.05, 1.0)
        r1, rn = rate(1, link, r_c, server), rate(n, link, r_c, server)
        if rn > r1 * (1 + 1e-9):
            extra = (n - 1) * delta
            s_star = break_even_size(extra, r1, rn)
            need(close(total_time(s_star, 1, t0, delta, link, r_c, server),
                       total_time(s_star, n, t0, delta, link, r_c, server)), "the two times are equal at S*")
            need(total_time(s_star * 0.5, n, t0, delta, link, r_c, server)
                 > total_time(s_star * 0.5, 1, t0, delta, link, r_c, server), "below S* one connection wins")
            need(total_time(s_star * 2, n, t0, delta, link, r_c, server)
                 < total_time(s_star * 2, 1, t0, delta, link, r_c, server), "above S* n connections win")
            if n * r_c <= cap:                  # below the cap R(n) = n r_c and S* = dT * r_c * n / (n - 1)
                need(close(s_star, extra * r_c * n / (n - 1)), "the closed form of the roadmap")
        # the best number of connections for a file of this size
        size = rng.uniform(1e5, 5e9)
        best = best_connections(size, t0, delta, link, r_c, server)
        brute = min(range(1, n_cap + 3), key=lambda m: (total_time(size, m, t0, delta, link, r_c, server), m))
        need(close(total_time(size, best, t0, delta, link, r_c, server),
                   total_time(size, brute, t0, delta, link, r_c, server)),
             f"best n={best}, brute force n={brute}")
    # E3: a 100 Mbit/s link and 2 MB/s per connection
    need(connections_to_reach_cap(12.5e6, 2e6) == 7 and rate(7, 12.5e6, 2e6) == 12.5e6
         and rate(6, 12.5e6, 2e6) == 12e6, "E3: seven connections reach 12.5 MB/s")
    eta = efficiency(1e6, 2e6, 0.08)                     # the server only serves pieces of 1 MB
    need(abs(eta - 0.862) < 5e-4 and math.ceil(12.5e6 / (2e6 * eta)) == 8, "E3: pieces of 1 MB need eight connections")
    # E4: eight connections, 0.15 s for each extra one, 2 MB/s each
    need(close(break_even_size(7 * 0.15, 2e6, 16e6), 2.4e6), "E4: S* = 2.4 MB")
    need(close(math.sqrt(100e6 / (2e6 * 0.15)), 18.257, rel=1e-4), "E4: 100 MB wants 18 connections if nothing caps them")


def list_schedule(jobs, n):
    """Graham's list scheduling: each job, in the order given, goes to the connection that frees first."""
    loads = [0] * n
    for job in jobs:
        loads[loads.index(min(loads))] += job
    return max(loads)


def optimal_makespan(jobs, n):
    best = sum(jobs)
    for assignment in itertools.product(range(n), repeat=len(jobs) - 1):
        loads = [0] * n
        loads[0] += jobs[0]                       # the first job on the first machine: the machines are alike
        for job, machine in zip(jobs[1:], assignment):
            loads[machine] += job
        best = min(best, max(loads))
    return best


@check("C14", "7.3", "list scheduling is within 2 - 1/n of optimal, LPT within 4/3 - 1/(3n); the tail is bounded")
def c14(rng):
    for _ in range(400):
        n = rng.choice([2, 2, 3, 3, 4])
        jobs = [rng.randint(1, 20) for _ in range(rng.randint(n, 7 if n < 4 else 6))]
        opt = optimal_makespan(jobs, n)
        need(max(sum(jobs) / n, max(jobs)) <= opt + 1e-9, "max(sum / n, p_max) is not a lower bound")
        ls = list_schedule(jobs, n)
        need(ls * n <= (2 * n - 1) * opt, f"list scheduling {ls} vs optimum {opt} on {n}: {jobs}")
        need(ls <= sum(jobs) / n + (1 - 1 / n) * max(jobs) + 1e-9, "the additive tail bound")
        lpt = list_schedule(sorted(jobs, reverse=True), n)
        need(lpt * 3 * n <= (4 * n - 1) * opt, f"LPT {lpt} vs optimum {opt} on {n}: {jobs}")
        shuffled = jobs[:]
        rng.shuffle(shuffled)
        need(list_schedule(shuffled, n) * n <= (2 * n - 1) * opt, "the bound holds for any order")
    # E8: a thousand 2 s segments on eight connections are within 0.7 % of the ideal 250 s
    need(list_schedule([2] * 1000, 8) <= 250 + (1 - 1 / 8) * 2 + 1e-9 and (1 - 1 / 8) * 2 / 250 == 0.007, "E8: the tail")


# --- 7.4 Putting the pieces together ----------------------------------------------------------------------

def choose_assembly(segments, connections, request_time, size, copy_rate):
    """'probe' (learn every segment's size first, then write each at its offset) or 'concatenate'."""
    probe_cost = math.ceil(segments / connections) * request_time
    copy_cost = size / copy_rate
    return ("probe" if probe_cost < copy_cost else "concatenate"), probe_cost, copy_cost


@check("C15", "7.4", "probing every segment costs ceil(K/n) * T_req; copying costs S / b_copy; the cheaper one wins")
def c15(rng):
    which, probe, copy = choose_assembly(1200, 8, 0.06, 1.5e9, 800e6)
    need(which == "concatenate" and close(probe, 9.0) and close(copy, 1.875), "E5: on an SSD, concatenate")
    which, probe, copy = choose_assembly(1200, 8, 0.06, 1.5e9, 60e6)
    need(which == "probe" and close(copy, 25.0), "E5: on a slow disk, probe")
    for _ in range(1000):
        k, n = rng.randint(1, 5000), rng.randint(1, 16)
        t_req, size, b = rng.uniform(0.01, 0.3), rng.uniform(1e6, 5e9), rng.uniform(2e7, 2e9)
        which, probe, copy = choose_assembly(k, n, t_req, size, b)
        need((which == "probe") == (probe < copy), "the rule must pick the cheaper cost")


def simulate_window(durations, n, window):
    """n connections take segments 0, 1, 2, ... in order, but a segment may start only while it is less than
    `window` places ahead of the oldest segment that is not yet written. Returns (finish, the most segments that
    waited behind the oldest, and how many were waiting each time a free connection had to idle)."""
    k = len(durations)
    in_flight, done, head, next_index, now = [], set(), 0, 0, 0.0
    waiting_peak, idle_at = 0, []
    while head < k:
        while len(in_flight) < n and next_index < k:
            if next_index >= head + window:
                idle_at.append(len(done))
                break
            heappush(in_flight, (now + durations[next_index], next_index))
            next_index += 1
        now, index = heappop(in_flight)
        done.add(index)
        while head in done:
            done.remove(head)
            head += 1
        waiting_peak = max(waiting_peak, len(done))
    return now, waiting_peak, idle_at


@check("C16", "7.4", "a window of W segments bounds what waits behind a slow one, and idles a connection only then")
def c16(rng):
    for _ in range(300):
        n = rng.randint(1, 8)
        window = rng.randint(n, 3 * n + 2)
        durations = [rng.choice([1, 1, 1, 1, 1, 5, 20]) * rng.uniform(0.5, 1.5) for _ in range(rng.randint(5, 80))]
        finish, peak, idle_at = simulate_window(durations, n, window)
        need(peak <= window - 1, f"{peak} segments wait with a window of {window}")
        need(all(w >= window - n + 1 for w in idle_at), "a connection idled before W - n + 1 segments were waiting")
        _, _, idle_unbounded = simulate_window(durations, n, len(durations) + 1)
        need(not idle_unbounded, "without a window no connection may idle")
        need(finish >= sum(durations) / n - 1e-9, "the finish cannot beat the ideal")


# --- 8. URLs expire ---------------------------------------------------------------------------------------

def refreshes_needed(transfer_time, ttl):
    return max(0, math.ceil(transfer_time / ttl - 1e-12) - 1)


@check("C17", "8", "a transfer of T seconds under a TTL needs ceil(T / ttl) - 1 refreshes; with none, e^(-lambda T)")
def c17(rng):
    for _ in range(2000):
        ttl = rng.randint(1, 50)
        t = rng.randint(1, 1000) / rng.choice([1, 4])
        elapsed, refreshed, valid_until = 0.0, 0, ttl
        while elapsed < t - 1e-12:                 # walk the transfer; at the boundary a new URL is needed
            elapsed += min(valid_until - elapsed, t - elapsed)
            if elapsed >= valid_until - 1e-12 and elapsed < t - 1e-12:
                refreshed += 1
                valid_until += ttl
        need(refreshed == refreshes_needed(t, ttl), f"T={t} ttl={ttl}: walked {refreshed}")
    need(refreshes_needed(500, 360) == 1 and close(4e9 / 8e6, 500), "E6: 4 GB at 8 MB/s under a 6 minute TTL")
    lam, t = 1 / 3600, 500
    p = math.exp(-lam * t)
    need(abs(p - 0.870) < 5e-4, "E6: a mean life of one hour and a 500 s transfer")
    trials = 40000
    hits = sum(1 for _ in range(trials) if rng.expovariate(lam) >= t)
    need(abs(hits / trials - p) < 4 * math.sqrt(p * (1 - p) / trials), f"Monte Carlo {hits / trials:.4f} vs {p:.4f}")


# --- 9. Which route first --------------------------------------------------------------------------------

def expected_cost(order, costs, successes):
    total, still_failing = 0.0, 1.0
    for i in order:
        total += still_failing * costs[i]
        still_failing *= 1.0 - successes[i]
    return total


@check("C18", "9", "routes tried in order of increasing cost / success probability cost the least on average")
def c18(rng):
    for _ in range(300):
        k = rng.randint(2, 6)
        costs = [rng.uniform(0.5, 600) for _ in range(k)]
        successes = [rng.uniform(0.05, 0.99) for _ in range(k)]
        ratio_order = sorted(range(k), key=lambda i: costs[i] / successes[i])
        best = min(itertools.permutations(range(k)), key=lambda o: expected_cost(o, costs, successes))
        need(close(expected_cost(ratio_order, costs, successes), expected_cost(best, costs, successes)),
             f"ratio order {ratio_order} is not optimal; best is {best}")
    costs, successes = [2, 5, 20, 600], [0.80, 0.90, 0.95, 0.99]
    need(close(expected_cost([0, 1, 2, 3], costs, successes), 4.0), "E7: 4.0 s on average")
    need(close(expected_cost([1, 0, 2, 3], costs, successes), 6.2), "E7: 6.2 s with the second route first")


@check("C19", "9", "after s successes and f failures from a uniform prior the estimate is (s + 1) / (s + f + 2)")
def c19(rng):
    steps = 20000
    for s, f in ((0, 0), (1, 0), (0, 1), (3, 1), (7, 12), (40, 2)):
        num = den = 0.0
        for i in range(steps + 1):                 # Simpson's rule on [0, 1]
            p = i / steps
            weight = 1 if i in (0, steps) else (4 if i % 2 else 2)
            density = p ** s * (1 - p) ** f
            num += weight * p * density
            den += weight * density
        need(abs(num / den - (s + 1) / (s + f + 2)) < 1e-7, f"s={s} f={f}")


# --- 10. Capture by playback --------------------------------------------------------------------------------

@check("C20", "10", "appended media covers the timeline when the union of [tfdt, tfdt + duration) does, seeks or not")
def c20(rng):
    for _ in range(500):
        timescale = rng.choice([1000, 90000])
        pieces, t = [], 0
        for _ in range(rng.randint(3, 40)):
            d = rng.randint(1, 8) * timescale
            pieces.append((t, t + d))
            t += d
        total = t
        appended = [rng.choice(pieces) for _ in range(rng.randint(len(pieces), 3 * len(pieces)))]   # seeks repeat some
        if rng.random() < 0.5:
            appended += pieces                                                # the player eventually plays everything
        holes = gaps_of(union_closed([(a, b - 1) for a, b in appended]), total)
        held = set(appended)
        missing = sum(b - a for a, b in pieces if (a, b) not in held)
        need(sum(b - a + 1 for a, b in holes) == missing, "missing ticks differ")
        need((not holes) == (held >= set(pieces)), "complete exactly when every piece was appended")


# --- runner ---------------------------------------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-k", metavar="TEXT", help="run only checks whose id or title contains TEXT")
    parser.add_argument("--list", action="store_true", help="list the checks and the section each one pins")
    args = parser.parse_args(argv)

    chosen = [c for c in CHECKS if not args.k or args.k.lower() in (c[0] + " " + c[2]).lower()]
    if args.list:
        for ident, section, title, _ in chosen:
            print(f"{ident}  {section:<5} {title}")
        return 0
    if not chosen:
        print("no check matches", args.k, file=sys.stderr)
        return 2

    passed = failed = 0
    for ident, section, title, fn in chosen:
        rng = random.Random(SEED ^ zlib.crc32(ident.encode()))
        try:
            fn(rng)
        except CheckFailed as e:
            failed += 1
            print(f"  ✘  {ident}  {title}\n       {e}")
        else:
            passed += 1
            print(f"  ✔  {ident}  {title}")
    print(f"\n  Passed: {passed}\n  Failed: {failed}")
    print("ALL CHECKS PASSED" if not failed else "CHECKS FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
