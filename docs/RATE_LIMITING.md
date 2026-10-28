# Rate limiting: being polite to servers

A segmented downloader is, to a server, a client that opens many connections at once. Many servers
object: public speed-test hosts, CDNs and anything behind `nginx limit_conn` answer the surplus with
`HTTP 429 Too Many Requests` or `503 Service Unavailable`, often with a `Retry-After` header. A
naive downloader that retries each refused segment on its own timer keeps hitting the limit until its
last retry, and then the download fails.

FluxDM treats a refusal as **information about the server**, not as an error of the connection. It
learns how many connections the server tolerates, queues the surplus instead of hammering, honours
`Retry-After`, and keeps going for as long as the server is making progress for it.

- [Design](#design)
- [The host gate](#the-host-gate)
- [Patience](#patience)
- [The probe](#the-probe)
- [What you see](#what-you-see)
- [Configuration](#configuration)
- [Failure semantics](#failure-semantics)
- [How it is tested](#how-it-is-tested)
- [Limitations](#limitations)

## Design

Three rules shape everything:

1. **The limit belongs to the server, not to the download.** Two downloads from one host share what
   that host tolerates, so the policy is process-wide. It lives in one place,
   [`core/src/hostgate.c`](../core/src/hostgate.c), which is pure bookkeeping: it never blocks, logs or
   touches the network, and time is passed in, so the whole policy is deterministic and unit-testable.
2. **A refusal is evidence of a limit.** When a connection is refused while *k* other connections to
   the host are open, the server has just shown that it accepts about *k*. The allowance drops to
   that, not to some fraction of a guess.
3. **Patience is measured against silence, not against the queue.** A host that is feeding any of
   our connections is working. A segment waiting for its turn behind the host's limit has lost nothing,
   and must not fail because its first refusal was a long time ago.

The shape is the one TCP uses to find the capacity of a link, applied to a server's patience: back off
at once to what was demonstrated, then probe upward slowly (additive increase, multiplicative-ish
decrease).

## The host gate

State is kept per `host:port` in a table of 64 entries (`GATE_TABLE`); a new host takes a free row or
evicts the stalest idle one. If every row is busy the connection runs ungated, since losing the policy
beats refusing to download. Entries are case-insensitive on the host name.

### Tunables

| Constant | Value | Meaning |
|---|---:|---|
| `HOSTGATE_OPEN` | 128 | Allowance of a host that has not complained |
| `HOSTGATE_MIN` | 1 | The allowance never drops below this: a host is never starved completely |
| `HOSTGATE_POLL_MS` | 100 ms | How long a segment without a slot sleeps before asking again |
| `HOSTGATE_CALM_MS` | 20 s | Quiet time after a refusal before the allowance may grow |
| `HOSTGATE_RAISE_MS` | 5 s | Growth rate once calm: +1 connection per interval |
| `HOSTGATE_GAP_MS` | 150 ms | Spacing of new connections while the host is unhappy |
| `HOSTGATE_FORGET_MS` | 300 s | An idle host's complaints are forgotten after this |
| `HOSTGATE_SETTLE_MS` | 2 s | Quiet time after a cut before it is worth telling the user |
| `HOSTGATE_PATIENCE_MS` | 300 s | Default patience (`--patience`) |

### Asking for a slot

Every connection a segment wants goes through `flux_gate_acquire`:

```
entry = find or create (host, port)
forget old complaints if the host has refused, nobody is connected,
    and it has been quiet for 300 s

if now < paused_until:                       # a Retry-After is running
    wait until it ends
unhappy = has_refused and now < last_refusal + 20 s

# somebody wants a slot the allowance does not cover:
# if the host has been calm for a while, try one more
if active >= limit and limit < 128 and not unhappy and now >= last_raise + 5 s:
    limit += 1

if active >= limit:                          wait 100 ms
elif unhappy and now < next_start:           wait until next_start     # 150 ms spacing
else:
    if unhappy: next_start = now + 150 ms
    active += 1                              # granted
```

Waiting is **not a failure** and spends no retry: the segment goes to the `Retrying` state with the
gate's delay and asks again.

### A refusal

When a connection that holds a slot is answered `429` or `503`, `flux_gate_throttled` is called before
the slot is released:

```
others    = active - 1
allowance = max(1, min(limit, others))       # what the server just demonstrated it accepts
limit     = allowance
last_refusal = last_raise = now              # growth restarts from here
if Retry-After: paused_until = max(paused_until, now + Retry-After)
```

So a server that cuts at 3 teaches the gate `limit = 3` within one wave of refusals, and the
remaining segments queue for those three slots instead of retrying on their own clocks.

### Healing

The allowance grows again when a cut was too deep or a limit was a one-off hiccup. Growth needs all of
the following: someone is actually waiting for a slot, the host has been calm for 20 s, and 5 s have
passed since the allowance last changed. It then grows by one. A new refusal at the higher level cuts
it back to what is demonstrated, and the cycle repeats. A host that has not been talked to for 300 s
is forgotten entirely.

## Patience

Patience is how long a download keeps waiting for a refusing server before it gives up (default
300 s). The clock measures the **host's silence**:

```
waited = now - max(refused_since, host.last_served)
```

- `refused_since` is the first refusal of the current unbroken run for that segment. It resets when the
  segment makes real progress (`SEG_RETRY_RESET_BYTES` = 16 KiB) or gets a real response.
- `host.last_served` is stamped, lock-free, by every segment of every download to that host each time
  a read delivers body bytes (`flux_gate_served` in the segment's pump).

A segment queued behind the host's limit while the host streams to its siblings therefore keeps its
full patience. Only when the host serves nobody does the clock run.

On each refusal a segment computes:

```
if waited >= patience:                          fail   "the server kept refusing for N s"
elif Retry-After > patience - waited:           fail   "the server asks us to wait N s"
else:
    delay = max(backoff(refusals), Retry-After)
    delay = min(delay, patience - waited)       # never sleep past the deadline
    retry in `delay`
```

`backoff(n)` is 1 s, 2 s, 4 s, ... capped at 30 s, with ±12 % jitter derived from the segment index
(`flux_gate_backoff_ms`) so sibling segments do not return in lockstep.

A `Retry-After` longer than the patience that is left fails **at once** and says what the server
asked for, instead of sleeping for minutes before failing anyway.

Setting patience to `0` restores fail-fast behaviour: the first refusal ends the download.

## The probe

The probe (the first request of every download) is refused too, and is treated the same way. It holds
no slot, so it reports through `flux_gate_note_refusal`; it honours another download's `Retry-After`
for the same host (`flux_gate_pause_ms`) before even trying; it uses the same silence-based patience and
back-off; and a refusal does not use up the attempts kept for real network failures. Any other
retryable status (`408`, `425`, `500`, `502`, `504`) or transient network error gets `PROBE_ATTEMPTS`
(4) tries with a doubling delay, and a `Retry-After` on those is obeyed up to 10 s.

All waiting happens on `poll()` loops that watch the worker's wake-up descriptor, so **pause and
cancel are immediate** even in the middle of a `Retry-After: 30`.

## What you see

Each piece of news about a host is logged **once**, however many segments saw it. A wave of 24
refusals is two lines, not 24:

```
[fluxdm-core] speedtest.example.net:443 is refusing connections (HTTP 429 Too Many Requests) — backing off
[fluxdm-core] speedtest.example.net:443: using at most 3 connections at a time for now
```

- The first line appears when a host that was not refusing starts to (`complaint`). With a
  `Retry-After` of a second or more it adds "it asks us to wait N s".
- The second appears when the allowance has stopped falling for 2 s (`settled`), and only if it
  differs from what the user was last told: the allowance dipping from 3 to 2 and back again as the
  gate probes upward is not news.

Per-refusal detail is available at log level 2 (`--log 2`).

## Configuration

| Where | Setting | Default |
|---|---|---|
| Core CLI | `--patience <seconds>`, range 0 to 86400 (anything else is refused with exit status 2) | 300 |
| Daemon | `fluxdm.core.patience-seconds` in `application.properties`, passed to the core | 300 |

The gate's tunables are compile-time constants in `core/include/hostgate.h`.

## Failure semantics

When patience runs out the segment fails, and the download ends in `ERROR` with the wire code
`SERVER` (8) and a message that says why, for example `HTTP 429 Too Many Requests (the server kept
refusing for 300 s)`. Progress is kept: the checkpoint is written, so the download can be resumed
later when the server is willing. Ordinary failures (resets, timeouts, `5xx`) are separate: they spend
the segment's retry budget (`SEG_MAX_RETRIES` = 8) as described in
[ARCHITECTURE.md](ARCHITECTURE.md#23-segments), while pushback never does.

## How it is tested

The policy is covered at three levels:

- **Unit tests** (`tests/test_unit.c`) drive the gate with an injected clock: allowance cuts and
  floors, spacing, healing only while someone waits, forgetting, `Retry-After` pauses, the patience
  clock (silence versus queueing), the once-only news, and back-off bounds and jitter.
- **Behaviour tests** (`tests/test_core.py`) run the real core against a scriptable mock server that
  can cap concurrent connections, answer `429` / `503` with `Retry-After`, hold connections open, and
  refuse on a script. They check, among others: a 3-connection server still delivers 8 segments intact,
  never sees more than 3 in flight, and refuses only a handful of times; `Retry-After` is honoured; a
  server that never stops refusing fails after about the patience (not after a retry count) with an
  explanatory message; a segment queued longer than the patience behind a host that is feeding its
  sibling still completes; two downloads from one host share what it tolerates; a wave of refusals
  produces one complaint line and at most two "settled" lines; pause and cancel are prompt during a
  long `Retry-After`; and no files are left behind.
- **Mutation checks.** These tests were validated by injecting deliberate bugs into the gate, the
  segment and the probe (ignoring `Retry-After`, starting the patience clock at the wrong moment, not
  counting the adopted probe connection, and so on) and requiring that a test fails for each one. See
  [TESTING.md](TESTING.md#test-quality).

## Limitations

- State is per process and in memory: a restarted core starts with a clean slate and relearns.
- The table tracks 64 hosts at once; beyond that, new busy hosts run ungated.
- Limits are learned per `host:port`, not per IP address or per CDN edge.
- The daemon and the UI do not yet show a dedicated "waiting for server" state; a download that is
  queued behind a host's limit reports as running with no progress. See [ROADMAP.md](ROADMAP.md).
