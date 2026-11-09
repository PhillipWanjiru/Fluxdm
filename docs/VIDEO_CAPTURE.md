# Video capture: taking what the browser plays, without an extractor

> **Status: a design with an executable check.** The extension, the daemon and the core do none of this
> yet; [section 13](#13-build-order) says in what order they will. What exists is
> [`tests/capture_model_check.py`](../tests/capture_model_check.py), which runs every formula below
> (checks `C01`–`C20`) and asserts every worked example (`E1`–`E9`), and
> [`tests/mutation_capture.py`](../tests/mutation_capture.py), which breaks each formula in turn to show
> that the check notices. The numbers in the worked examples are examples, not measurements; what has to
> be measured is listed in [section 14](#14-what-has-to-be-measured).

- [1. The decision](#1-the-decision)
- [2. Model and principles](#2-model-and-principles)
- [3. What the extension can see](#3-what-the-extension-can-see)
- [4. Detection is a decision under cost](#4-detection-is-a-decision-under-cost)
- [5. Which requests are one stream](#5-which-requests-are-one-stream)
- [6. Manifests: HLS and DASH](#6-manifests-hls-and-dash)
- [7. Fetching: how fast, and in what order](#7-fetching-how-fast-and-in-what-order)
- [8. URLs expire](#8-urls-expire)
- [9. Choosing among routes](#9-choosing-among-routes)
- [10. Capture by playback](#10-capture-by-playback)
- [11. The hand-off is a trust boundary](#11-the-hand-off-is-a-trust-boundary)
- [12. What this does not do](#12-what-this-does-not-do)
- [13. Build order](#13-build-order)
- [14. What has to be measured](#14-what-has-to-be-measured)
- [15. References](#15-references)

## 1. The decision

FluxDM's first video prototype asked an *extractor* (yt-dlp, behind a Python service) to turn the address
of a page into the addresses of its media. FluxDM drops that route and will not have an extractor.

It will **capture** instead. A browser extension watches the requests the page's own player makes,
recognises the ones that carry media, and gives FluxDM what it needs to fetch the same bytes: the address,
the headers the browser sent and, for adaptive streams, the manifest. The core then downloads them on its
own connections, resumes them, applies its rate limits, and the daemon assembles the file.

### 1.1 Extract or capture

| | Extract (yt-dlp and similar) | Capture (this design) |
|---|---|---|
| Who works out the media address | A per-site reimplementation of what the site's player script does: its requests, its parsing, its signature and token functions | The page's own player, running in the browser |
| What breaks it | Any change to a site's player or API, site by site | A change in how browsers expose requests, or in the streaming formats: infrequent, and the same for every site |
| What it needs besides the downloader | The extractor, kept current, and for some sites a JavaScript runtime | An extension in the browser the person already uses |
| Sees what the signed-in person sees | Only with exported cookies | Yes: the player's requests already carry the person's cookies, and the extension passes them on |
| A site nobody has written an extractor for | Not served | Served, if the page plays it over ordinary HTTP |
| Pages the browser has not played | Served | Not served: nothing was requested |

The second row is the argument. The upkeep of an extractor grows with the number of sites and with how
often each one changes; capture depends on a few mechanisms (HTTP, HLS, DASH, Media Source) that change
slowly. The last row is its price.

### 1.2 What IDM documents, and what it does not

The question was how Internet Download Manager (IDM) finds videos, since it is older than yt-dlp. Its own
FAQ describes the mechanism in general terms:

- "Our extension examines all sent and received data from websites to detect download links" and "it also
  parses web page source and script data to extract multimedia content", and "analyzes document objects in
  tabs and watches your navigation between web pages" (the extension's permissions page,
  [chrome_perms](https://www.internetdownloadmanager.com/register/new_faq/chrome_perms.html)).
- The native-messaging permission is needed "to transfer captured downloads to IDM desktop program"
  (same page).
- On what it will not do: "IDM will not download protected streams with copy protection measures like
  encryption" ([video11](https://internetdownloadmanager.com/register/new_faq/video11.html)), and "Some
  videos (for example private videos on YouTube) may use protected protocol that IDM does not support for
  legal reasons" ([video1](https://data.internetdownloadmanager.com/register/new_faq/video1.html)).

| | |
|---|---|
| **Documented** | Detection by examining the traffic and the page; a desktop program that receives what the extension captured; protected streams are not downloaded |
| **Inferred** (from the above and from what the platform allows) | Detection is observation, not per-site extraction: nothing in the FAQ describes an extractor, and the permissions it asks for are the ones observation needs |
| **Not known** | IDM's detection rules and thresholds; how it fetches streams whose range is in the address; how it picks among variants |

This design is therefore derived from the interfaces the platform documents (section 3) and the formats'
specifications (section 6), not from IDM's internals, which are not public.

### 1.3 Scope and responsibility

FluxDM downloads what the person's own browser is allowed to fetch. It does not circumvent copy
protection (section 12). Whether a download is allowed by a site's terms or by copyright is the
person's responsibility, as it is for the browser's own "save as".

## 2. Model and principles

A page's player issues requests. Each one is observed as

```
r = (t, method, u, H, status, G, type, initiator)
     time  verb   URL  request headers, response status, response headers, resource type, origin
```

and a **stream** is a set of requests that together carry one piece of media. What a player requests
decides what FluxDM has to do:

| Kind | What the player requests | What FluxDM does |
|---|---|---|
| File | One address, whole or by `Range` header | Fetches it, resumably |
| Chunked file | One path many times, each for a byte range, the range in the query string or in a header | Recognises one stream ([section 5](#5-which-requests-are-one-stream)), fetches the whole span |
| HLS | A master playlist, media playlists, segments ([6.1](#61-hls)) | Parses, picks a variant, fetches segments in order |
| DASH | A manifest (MPD), initialisation segments, media segments ([6.2](#62-dash)) | Parses, picks representations, fetches, merges tracks |
| MSE feed | Bytes the page appends to a `SourceBuffer` ([section 10](#10-capture-by-playback)) | Copies what the player was given |

Five principles hold for all of it.

1. **Player equivalence.** FluxDM asks for what the player asked for: the same address template, the same
   host, the same allow-listed headers, pieces no larger than the largest the player asked for unless a
   probe shows a larger one is served exactly. It must not look like something else to the server, and it
   must not need to know why a parameter is there.
2. **No circumvention.** An encrypted stream whose key the browser gets from a licence server (DRM) is not
   captured, and neither is the decrypted output of one ([section 12](#12-what-this-does-not-do)).
3. **Credentials stay small.** Captured cookies and headers live in memory for the life of the job, travel
   only over the loopback interface with the API token, and are sent only to the host they came from
   ([section 11](#11-the-hand-off-is-a-trust-boundary)).
4. **Offer, do not act.** A candidate is shown to the person; nothing is downloaded because a page asked
   the extension to.
5. **Fail closed.** A candidate that cannot be explained (an unknown manifest feature, a size that does not
   add up) is reported and left alone, not guessed at.

## 3. What the extension can see

Chrome's `webRequest` API is the observation point. Its reference
([webRequest](https://developer.chrome.com/docs/extensions/reference/api/webRequest)) says:

- "You must declare the "webRequest" permission in the extension manifest to use the web request API,
  along with the necessary host permissions." For sub-resource requests the extension needs access to both
  the requested address and its initiator.
- Request types include `media` and `xmlhttprequest`. A player that lets a `<video>` element load the file
  itself is expected to show up as `media`; one that fetches segments with `fetch` or `XMLHttpRequest` and
  feeds Media Source (most adaptive players) as `xmlhttprequest`. Detection must therefore not depend on
  `type` alone, and the first recorded sessions will show how far the expectation holds.
- `onSendHeaders`: "This event is informational and handled asynchronously." It observes; it does not
  allow modifying or cancelling the request. That is all capture needs.
- Of `webRequestBlocking`: "As of Manifest V3, this is only available to policy installed extensions."
  Capture does not need it.
- `Accept-Language`, `Accept-Encoding`, `Referer` and `Cookie` are listed as headers that "cannot be
  modified or removed without specifying 'extraHeaders'". Capture specifies it, because the cookies are
  what make a download behind a sign-in work.
- A further list of headers, `Authorization` and `Cache-Control` among them, is not provided to
  `onBeforeSendHeaders` at all. The design therefore assumes that **`Authorization` cannot be captured**;
  a stream that needs one is served by capture by playback ([section 10](#10-capture-by-playback)), which
  copies what the player was given however the player obtained it.

The existing extension skeleton declares the permissions `declarativeNetRequest`, `downloads`, `storage`
and `nativeMessaging`, and the host permission `<all_urls>`. Capture adds `webRequest`.

**What the extension cannot see: response bodies.** `webRequest` gives headers and metadata, not content.
So a manifest is recognised by its address and `Content-Type`, and its text is fetched again by FluxDM with
the captured headers: one extra request per manifest, made where it can be retried, bounded and logged. The
alternative, hooking `fetch` and `XMLHttpRequest` in the page to read bodies, is more invasive and breaks
more often; it is kept for the playback fallback ([section 10](#10-capture-by-playback)).

Two facts have to be confirmed on a real browser before the extension is built on them
([section 14](#14-what-has-to-be-measured)): that `Cookie` and `Referer` are present in `onSendHeaders`
when `extraHeaders` is requested from a Manifest V3 service worker, and that `Authorization` is absent
there, as the reference suggests.

## 4. Detection is a decision under cost

### 4.1 Evidence

Each observed request yields features. None is conclusive; together they are.

| Evidence | Comes from | What it says |
|---|---|---|
| Resource type `media` | `details.type` | The browser itself loaded it as media. Absent for Media Source players |
| `Content-Type` `video/*`, `audio/*`, `application/vnd.apple.mpegurl`, `application/x-mpegurl`, `application/dash+xml` | `onHeadersReceived` | The strongest header signal. `application/octet-stream` is weak |
| Path extension `.mp4 .webm .m4s .ts .m3u8 .mpd .mp3 .m4a` | The address | Cheap, and often hidden by a CDN |
| Status `206` and a `Content-Range` | The response | A server handing out pieces of something large; the total after the `/` is its size |
| Size | `Content-Length`, or the `Content-Range` total | Media is megabytes; a few kilobytes is rarely worth offering |
| Repetition with a position parameter | Many requests to one path ([section 5](#5-which-requests-are-one-stream)) | One chunked file |
| Magic bytes | A `Range: bytes=0-1023` probe by FluxDM, the first kilobyte | `ftyp` at offset 4 (ISO base media), `1A 45 DF A3` (WebM, Matroska), `0x47` every 188 bytes (MPEG-TS), `#EXTM3U` (HLS), `<MPD` after the XML declaration (DASH), `OggS`, `ID3` |

### 4.2 When to offer a candidate

Let `p` be the probability that a candidate is media, `c_fp` the cost of offering one that is not (a
useless entry, a click, a little distrust) and `c_fn` the cost of not offering one that is (the video the
person came for). Then

```
expected cost if offered        (1 − p) · c_fp
expected cost if not offered          p · c_fn

offer  ⇔  (1 − p) · c_fp < p · c_fn
       ⇔  p > τ = c_fp / (c_fp + c_fn)
       ⇔  ln( p / (1 − p) ) > ln( c_fp / c_fn )
```

so the threshold is set by the costs, not by taste. Check `C01` compares the rule with the expected costs
over thousands of random draws.

**E1.** If a missed video is five times as bad as a useless entry (`c_fn = 5·c_fp`), then `τ = 1/6`:
offer anything that is at least one-in-six likely to be media.

### 4.3 Combining the evidence

With features `x_k` taken as independent given the class (naive Bayes) and a prior `π` that a request is
media, the posterior odds are the prior odds times one likelihood ratio per feature:

```
P(media | x) / P(other | x)  =  π / (1 − π)  ·  Π_k  LR_k          LR_k = P(x_k | media) / P(x_k | other)

ln(posterior odds)           =  ln( π / (1 − π) )  +  Σ_k  ln LR_k
```

The sum is what an implementation computes: one weight per piece of evidence, added to a bias. Check `C02`
shows that this equals the direct product form for random tables. Where features are correlated (a `.mp4`
path and a `video/mp4` type are not independent), the two would be counted twice; the remedy is to fit the
weights jointly (logistic regression has the same additive form), which changes how the weights are
obtained and not the decision rule.

**E1, continued.** A prior of 2 % and example ratios (not measurements): a path ending `.mp4` alone has
`LR = 30` and gives `p = 0.380`, which is above `1/6` and is offered; a bare `206` has `LR = 3` and gives
`p = 0.058`, which is not; a `video/mp4` type (`LR = 400`) with a `206` whose total exceeds 1 MB (`LR = 8`)
gives `p = 0.985`.

### 4.4 Hard rules, and what is left to measure

A recognised magic number or a manifest type is conclusive enough to be a rule and not a weight: such a
candidate is media, without further arithmetic. Everything else uses the weights, which are fitted on
recorded sessions of pages with and without video, with the costs `c_fp`, `c_fn` as the one setting a
person might change ("how eager to show candidates"). Precision and recall targets are set after the first
corpus exists, not before.

## 5. Which requests are one stream

### 5.1 The problem

Many players do not fetch a file in one request. They ask for it in pieces, and some put the range in the
query string rather than in a `Range` header: `…/video?id=42&range=0-1048575`, then
`…&range=1048576-2097151`, and so on. To a capture that sees only requests, these look like unrelated
addresses. They are one stream, and FluxDM has to fetch the whole of it.

### 5.2 What each parameter is for

Group the requests by host and path. For each query parameter `p` take its values across the `m` requests
of a group and compute

```
H(p) = − Σ_v q_v · log2 q_v        (q_v: the share of requests that carry value v)
v(p) = H(p) / log2 m               0 when every request carries the same value, 1 when each carries its own
                                   (taken as 0 when m = 1)
```

A parameter is then one of

- **identity**, if `v(p) ≤ 1/2`: few values, repeated. A constant (`v = 0`) is a token or an id. A parameter
  with a few values (an audio and a video track, `v` small) splits the group into streams. When every value
  is used equally often, `v ≤ 1/2` is the same as "at most `√m` distinct values" (check `C03`);
- **position**, if `v(p) > 1/2` and its values *tile a span*: as ranges `a-b`, sorted, each begins one byte
  after the previous one ends, repeats ignored; or as numbers, consecutive;
- **unresolved**, otherwise: a timestamp or a counter that differs on every request, or a position that the
  pieces seen do not prove (the player skipped ahead and left a hole, or two pieces overlap). It is left out
  of the key and not varied (principle 5). A value shaped like a byte range is settled by the probe of
  [5.4](#54-player-equivalence-in-practice); anything else is offered only as the requests seen.

The **stream key** is the host, the path and the identity parameters; position and unresolved parameters
are dropped. Identity is tested first on purpose: reading a track selector (`stream=0`, `stream=1`) as a
position would merge two different streams and fetch the wrong bytes, while reading a position as an
identity only splits one stream into several candidates. Check `C04` separates a shuffled tiling with a
repeated piece (a position) from a tiling with a hole, an overlap, a run of timestamps, or a constant. The
threshold `1/2` is a heuristic and will be tuned on recorded sessions.

### 5.3 Range algebra

Observed ranges are sets of closed intervals of bytes. Their union is computed by sorting and one sweep,
`O(m log m)`; intervals that touch (`b + 1 = a'`) merge. With `S` the size, known from the part after the
`/` of a `Content-Range: bytes a-b/S` (or the `Content-Length` of a `200`):

```
covered   = union of the observed [a, b]
missing   = [0, S − 1] \ covered                      the gaps
requests  = Σ over gaps  ⌈ |gap| / w ⌉                requests of at most w bytes over bytes not yet held
```

Check `C05` compares the union, the gaps and the request count with a byte-by-byte bitmap on 1,500 random
inputs, and parses the three forms of `Content-Range` (`bytes a-b/S`, `bytes a-b/*` and `bytes */S`). A
hole of 100 bytes filled in pieces of at most 64 takes two requests.

FluxDM re-fetches everything, because the extension never sees the bytes (section 3). The algebra is used
to find `S`, to recognise a stream whose pieces tile a span, to resolve the byte ranges inside manifests
([6.1](#61-hls)) and, in the core, to keep track of what is held, which is the bookkeeping the downloader
already does.

### 5.4 Player equivalence in practice

For a `Range`-header stream the same address and any range is replayed. For a range-in-address stream the
parameter's form is replayed (`range=a-b`, or whatever the player used) with piece sizes up to the largest
observed. A larger piece is tried only by a probe: ask for it once, accept it if the answer is `206` (or
`200`) with exactly the length asked for, fall back to the observed size if not.

## 6. Manifests: HLS and DASH

### 6.1 HLS

HLS is specified in [RFC 8216](https://www.rfc-editor.org/rfc/rfc8216). A **master playlist** lists
variants (`EXT-X-STREAM-INF`) and alternative renditions (`EXT-X-MEDIA`: audio, subtitles); a **media
playlist** lists segments (`EXTINF` duration, then the address, relative to the playlist's own).

**Choosing a variant.** Each variant has a `BANDWIDTH`, which "represents the peak segment bit rate of the
Variant Stream", a `RESOLUTION`, `CODECS`, and perhaps an `AVERAGE-BANDWIDTH`, which "represents the average
segment bit rate" (§4.3.4.2). The peak is not the rate of the fastest segment. §4.1 defines it as the largest
bit rate of any contiguous set of segments whose total duration is between 0.5 and 1.5 times the target
duration, the bit rate of a set being its total size over its total duration, and the average of a playlist
as its total size over its duration. With `D = Σ EXTINF`:

```
size ≤ BANDWIDTH · D / 8              a ceiling to plan with, when every segment is itself a window
size ≈ AVERAGE-BANDWIDTH · D / 8      the estimate, exact when the declared average is exact
chosen variant = highest (RESOLUTION, BANDWIDTH) that meets the person's limits on height,
                 estimated size and codecs
```

The ceiling holds because a peak is at least an average: the playlist is a run of windows, and its rate is
the duration-weighted mean of theirs. It can fail when a short last segment (under half a target duration)
lies outside every window, and a declared value can simply be wrong, so it is a planning number, not a
guarantee. For a variant with separate audio, `BANDWIDTH` covers the largest combination of renditions, so
it bounds the video and the audio together. The free-space check uses the ceiling before anything is written.
Check `C09` models the windowed peak, verifies both relations on random segment lists in which each segment
is a window, and asserts the short-tail exception on a constructed case. **E9.** Two hours at a peak of
5 Mbit/s is at most `5·10⁶ · 7200 / 8 = 4.5 GB`; at an average of 3.2 Mbit/s the estimate is `2.88 GB`.

**Byte ranges.** `EXT-X-BYTERANGE:n[@o]` makes a segment `n` bytes of a larger resource. When `o` is
absent "the sub-range begins at the next byte following the sub-range of the previous Media Segment"
(§4.3.2.2), and a previous segment of the same resource must exist. Resolving them is a running sum; the
resolved ranges of one resource tile it (check `C06`), which is the same test as in section 5.2.

**Initialisation.** `EXT-X-MAP` names the initialisation section (for fragmented MP4, the `ftyp` and `moov`
boxes) that applies to every later segment (§4.3.2.5). It is fetched once and written first.

**Encryption.** With `METHOD=AES-128` each segment is encrypted whole, with AES-128 in CBC mode and PKCS7
padding, and CBC restarts on every segment boundary (§4.3.2.4), so each segment decrypts on its own, in any
order. When the key line has no `IV`, "the Media Sequence Number is to be used as the IV", by
putting "its big-endian binary representation into a 16-octet (128-bit) buffer and padding (on the left)
with zeros" (§5.2). The first segment's number is its `EXT-X-MEDIA-SEQUENCE` (§4.3.3.2; 0 when the tag is
absent), and "the Media Sequence Number of every other segment is equal to the Media Sequence Number of the
segment that precedes it plus one" (§3), so the IV of the segment at position `i` is `media_sequence + i`
written that way. Check `C07` pins this byte for byte. The key is fetched with the captured headers, and
decryption uses OpenSSL, which the core already links (`-lssl -lcrypto`). Only keys with
`KEYFORMAT="identity"`, the default, are used: any other key format (FairPlay, Widevine, PlayReady) is DRM
and is refused ([section 12](#12-what-this-does-not-do)), and `SAMPLE-AES`, which encrypts inside the
samples and needs a parser of its own, is left out of the first stages.

**Consistency.** Every `EXTINF`, rounded to the nearest integer, must not exceed `EXT-X-TARGETDURATION`
(§4.3.3.1). A playlist that breaks that is malformed and is reported, not repaired. The smallest valid
target is the largest rounded duration (check `C08`).

A playlist with `EXT-X-ENDLIST` is complete (video on demand). One without is live, and is out of scope
for the first stages ([section 13](#13-build-order)).

### 6.2 DASH

A DASH manifest (MPD, ISO/IEC 23009-1) is a tree: periods, adaptation sets (a kind of track), and
representations (an encoding of it). Segment addresses come from one of three places: a `SegmentTemplate`
with a fixed duration, a `SegmentTemplate` with a `SegmentTimeline`, or a `SegmentBase` / `SegmentList`
(byte ranges inside one file, found through its `sidx`).

With a timeline of entries `S(t, d, r)` (start, duration, and a repeat count: the entry stands for `r + 1`
segments; a negative `r` runs to the next entry's `t`, or to the end of the period), in units of the
timescale:

```
N              = Σ (r_i + 1)                                   the number of segments
r = −1:        r_i + 1 = ⌈ (until − t_i) / d_i ⌉               until: the next entry's t, or the period's end
start of k-th  = t_i + j · d_i        (k counted across entries, j within entry i)
$Number$       = startNumber + k                               $Time$ = start of the k-th segment
fixed duration d:   N = ⌈ period · timescale / d ⌉             the last segment is the short one
```

Check `C10` expands random timelines, with discontinuities, open-ended repeats and live-style non-zero
starts, and compares the expansion with these formulas. Representation choice follows the same rule as the
HLS variant choice. Dynamic (live) manifests are out of scope for the first stages.

### 6.3 Separate tracks

Adaptive players fetch video and audio as separate streams. FluxDM fetches both and they have to be
combined. If each track is started at the segment that contains a time `T₀` (for a clip, or to resume),
and the longest segments are `Δv` and `Δa` seconds, then the start of the video segment minus the start of
the audio segment `s_v − s_a` satisfies

```
− Δv  <  s_v − s_a  <  Δa             so   | s_v − s_a |  <  max(Δv, Δa)
```

because each start lies less than one segment before `T₀`. Check `C11` tests the bound on random segment
grids, including the sign convention. The skew is harmless if the combiner places samples by their own
timestamps (`tfdt` in fragmented MP4, the presentation timestamps in MPEG-TS), which is what any muxer does;
it matters only for a combiner that aligns by file start, which would leave up to `max(Δv, Δa)` of offset.

**Output.** A single-track HLS or DASH stream is written as it arrives: initialisation first, then
segments in order (MPEG-TS concatenates; fragmented MP4 is a valid, if fragmented, `.mp4`). Combining
separate tracks, or turning fragmented MP4 into a flat one, needs a remuxer. FluxDM will use `ffmpeg` for
that when it is installed (`-c copy`, no re-encoding) and says so when it is not; the two tracks are then
kept as two files. This optional dependency is a decision to confirm
([section 14](#14-what-has-to-be-measured)).

## 7. Fetching: how fast, and in what order

Notation, as in [ROADMAP.md](ROADMAP.md) item 7 (the benchmark against curl, wget and aria2 measures
these):

| Symbol | Meaning |
|---|---|
| `S` | Bytes of the stream; `K` segments |
| `n` | Connections |
| `B` | The link's rate; `r_c` one connection's rate; `r_s` any cap the server applies |
| `T_req` | The time from sending a request on a warm connection to the first payload byte |
| `w` | Bytes asked for in one request |
| `T0`, `δ` | Start-up of the first connection; the extra start-up of each further one |
| `R(n)` | The rate with `n` connections |

### 7.1 What a request costs

A connection spends `w / r_c` seconds moving a piece and `T_req` waiting for the next, so its efficiency is
`η(w) = (w / r_c) / (w / r_c + T_req)`. The smallest piece that reaches a target `η₀` is

```
w_min = η₀ · r_c · T_req / (1 − η₀)
```

(check `C12`). **E2.** At 5 MB/s per connection and `T_req = 80 ms`, 90 % needs `w ≥ 3.6 MB`. Pieces
smaller than that keep a connection waiting for more than a tenth of its time. If the largest piece the
player asked for is smaller, the probe of [5.4](#54-player-equivalence-in-practice) looks for a larger one,
and when the server will not serve one, the loss is made up with connections (7.2).

### 7.2 How many connections

```
R(n) = min( B,  n · r_c,  r_s )         concave and non-decreasing: it rises by r_c per connection,
                                        then stops at the cap, at n_cap = ⌈ min(B, r_s) / r_c ⌉
T(S, n) = T0 + (n − 1)·δ + S / R(n)
```

Opening a connection costs `δ` that does not overlap with the others (the worst case; a core that opens
connections concurrently has a smaller `δ`, which the benchmark measures). From that:

- **Break-even.** One connection finishes first below a certain size and `n` connections above it; the two
  times are equal at

  ```
  S* = ΔT · R(1) · R(n) / ( R(n) − R(1) )         ΔT = (n − 1)·δ
  S* = n · δ · r_c                                 below the cap, where R(n) = n · r_c
  ```

  The second line is the roadmap's `ΔT_setup · r_c · n / (n − 1)`.
- **Best `n` for a file of `S` bytes.** Below the cap, `T` is `(n − 1)·δ + S / (n · r_c)` plus a constant,
  which is convex in `n` with its minimum at
  `n_opt = √( S / (r_c · δ) )`. The best integer is one of `⌊n_opt⌋`, `⌈n_opt⌉`, `n_cap − 1` and `n_cap`.
  So the number of connections should grow with the file, and stop at the cap.
- **Small pieces.** If the server only serves pieces of `w`, a connection's rate is `r_c · η(w)`, and the
  cap is reached at `⌈ min(B, r_s) / (r_c · η(w)) ⌉` connections.

Check `C13` verifies the concavity, `n_cap`, both break-even forms and that the best integer matches a
brute-force search over every `n` from 1 to just past the cap, on 300 random links. **E3.** On a
100 Mbit/s link (12.5 MB/s) with 2 MB/s per connection, seven connections reach the link
(`⌈12.5 / 2⌉ = 7`); if only 1 MB pieces are served and `T_req` is 80 ms, `η = 0.862` and eight are needed
(`⌈12.5 / (2 · 0.862)⌉ = 8`). **E4.** With `δ = 0.15 s`, eight connections beat one above
`S* = 8 · 0.15 · 2 MB = 2.4 MB`, and a 100 MB file would want `√(10⁸ / (2·10⁶ · 0.15)) = 18` connections
if nothing capped them.

### 7.3 The order of segments, and the tail

Segments are handed out in playlist order, each to the connection that frees first: Graham's *list
scheduling*. For jobs of sizes `p_i` on `n` connections, with `OPT` the best possible finishing time,

```
OPT  ≥  max( Σ p_i / n,  p_max )
list scheduling   ≤  ( 2 − 1/n ) · OPT        and        ≤  Σ p_i / n  +  ( 1 − 1/n ) · p_max
longest first     ≤  ( 4/3 − 1/(3n) ) · OPT
```

Check `C14` finds `OPT` by exhaustive search on 400 small instances and tests each bound, for any order.
**E8.** A thousand 2-second segments on eight connections finish within `(1 − 1/8) · 2 s` of the ideal
250 s, which is 0.7 %. The tail is a rounding error when there are many small segments, so there is no
need to reorder by size, which would fight the in-order writing of 7.4.

### 7.4 Putting the file together

**Where to write each segment.** Either learn every segment's size first (a `HEAD` or a one-byte range per
segment, in parallel) and write each at its offset, or write each into its own piece and concatenate:

```
probe        ⌈ K / n ⌉ · T_req
concatenate  S / b_copy                  b_copy: the rate of a sequential read and write of the whole
```

The cheaper one wins (check `C15`). **E5.** For `K = 1200`, `n = 8`, `T_req = 60 ms` and `S = 1.5 GB`,
probing costs 9 s. Concatenating costs 1.9 s on an SSD at 800 MB/s, so concatenate; on a disk that copies at
60 MB/s it costs 25 s, so probe. When segments are written in order there is neither cost, and that is the
normal case:

**In-order writing and its window.** Connections finish out of order, so finished segments wait for the
oldest unfinished one. To bound memory a segment may start only while it is fewer than `W` places ahead of
the oldest segment not yet written. Then, with `n` connections:

- the finished segments waiting behind the oldest number at most `W − 1`, so the data held back is at most
  `(W − 1) · p_max`;
- the window keeps a connection from starting a segment only when exactly `W − f` finished segments are
  waiting (`f`: the segments still in flight), which is at least `W − n + 1`: a slow segment holds up the
  others only after that many have gone ahead of it;
- with no window (`W ≥ K`) the window never holds a connection back.

Check `C16` simulates 300 random schedules with slow segments and tests all three. Counting the segments
still arriving, the window holds at most `W · p_max`, so a memory budget `M` gives `W = ⌊ M / p_max ⌋`,
and never less than `n`.

## 8. URLs expire

Signed addresses stop working. Some carry their expiry (`Expires`, `expire`, `X-Amz-Date` with
`X-Amz-Expires`, `se`, `X-Goog-Expires`; a heuristic, site by site, never relied on alone). Two models
bound what a transfer of `T = S / R(n)` seconds faces:

```
fixed lifetime L, refreshed on demand     refreshes = ⌈ T / L ⌉ − 1
memoryless expiry, rate λ, no refresh     P(finish before it expires) = e^(−λ·T)
```

Check `C17` walks transfers against a lifetime and compares the count, and samples the exponential model.
**E6.** A 4 GB file at 8 MB/s takes 500 s; under a 6-minute lifetime that is one refresh; with a mean life
of an hour and no refresh the chance of finishing is `e^(−500/3600) = 0.87`.

How to refresh is the hard part, because FluxDM cannot mint addresses; only the player can. In order of
cost: fetch the manifest again with the captured headers (new segment addresses often come with it);
fetch the stream's first address again; ask the extension to capture again, which needs the tab still open.
If even the fastest possible transfer, `S / R(n_max)`, is longer than the lifetime left at capture, the
person is told at once that the address will have to be refreshed, instead of finding out at 90 %.

## 9. Choosing among routes

For one candidate there may be several ways to fetch it: replay the address directly (cost `c₁`, success
probability `p₁`), fetch the manifest and its segments, ask the extension to capture again, capture by
playback. They are tried in an order until one works. With independent successes, the expected cost of
trying routes in the order `σ` is `c_σ1 + (1 − p_σ1)·c_σ2 + (1 − p_σ1)(1 − p_σ2)·c_σ3 + …`, and swapping
two neighbours `i, j` changes it so that `i` first is better exactly when `c_i / p_i < c_j / p_j`. The best
order is therefore by increasing `c / p`; check `C18` confirms it against all permutations of up to six
routes. **E7.** With costs of 2, 5, 20 and 600 seconds and success probabilities 0.80, 0.90, 0.95 and
0.99, the ratios are 2.5, 5.6, 21 and 606 and the expected cost is 4.0 s; starting with the second route
costs 6.2 s.

The probabilities are not constants: they depend on the host. FluxDM keeps, for each host and route, the
successes `s` and failures `f`, and uses the posterior mean from a uniform prior,
`p̂ = (s + 1) / (s + f + 2)` (check `C19`, against numerical integration). A route that has never been tried
starts at one half.

## 10. Capture by playback

Some streams cannot be fetched again: the address is one-time, or the page builds the data itself. What
the player is given can still be copied: a script running in the page wraps `SourceBuffer.appendBuffer`
([Media Source Extensions](https://www.w3.org/TR/media-source-2/)) and keeps each buffer, in the order
appended, per track. For ISO base media the first buffer is the initialisation segment (`ftyp` + `moov`),
and each later one holds a `moof` with a `tfdt` (the decode time) and the samples
([byte stream format](https://www.w3.org/TR/mse-byte-stream-format-isobmff/)).

Seeking makes the player append again, so the result is an interval problem, as in section 5.3: the union
of `[tfdt, tfdt + duration)` per track, in ticks, is complete when it covers `[0, D · timescale)`, and the
uncovered ticks divided by the timescale (to make them seconds) and by the playback speed are the time still
to wait. Check `C20` shows that completeness and the uncovered amount follow from the union however often
and in whatever order pieces are appended.
Cost is real time, which is why it is the last route in section 9, and what the player's buffering allows.

The same hook is where DRM shows itself: appended data that carries `pssh` or `tenc` boxes, or a page that
requests a licence, is encrypted. It is detected and refused (section 12).

## 11. The hand-off is a trust boundary

The extension sees every request the browser makes. The daemon is a local web server that anyone on the
machine can reach, and the person's cookies are about to cross between them.

| Threat | Rule |
|---|---|
| A page makes the extension offer an address that reaches inside: `http://127.0.0.1:7171/…`, a private address, `file:`, `ftp:` | Only `http:` and `https:`. Never the daemon's own host and port. A loopback or private target needs the person's confirmation, unless the page itself is at such an address |
| The captured cookies and headers leak | They go extension → native host → daemon over the loopback interface with the API token, are kept in memory for the life of the job, are never written to the database or the log, are attached only to requests for the host they were captured from, and are dropped on a redirect to another host. The core has no custom request headers today, so this is built with them (ROADMAP item 3) |
| The extension is a window on everything | It keeps only candidates, sends nothing else anywhere, has a switch to turn it off, and lists what it captured in the popup. No remote code, no analytics |
| A page floods the popup | At most a fixed number of candidates per tab, de-duplicated by stream key |
| A hostile manifest: a million segments, huge sizes, addresses that reach inside | Caps on segments, on declared sizes and on nesting. Each segment address passes the first rule. A free-space check against the upper bound of 6.1 before anything is written |
| A hostile title or address makes a file name | The same file-name rules as any other download |

## 12. What this does not do

- **DRM.** Content protected by Widevine, PlayReady or FairPlay, or by any key delivered through a licence
  server: not captured, and neither is its decrypted output. Of HLS's encryption, `AES-128` with an
  `identity` key is used ([6.1](#61-hls)); any other key format is refused, and `SAMPLE-AES` is left out of
  the first stages. This is the line IDM's FAQ says it draws (section 1.2); FluxDM draws it in the design,
  not in a setting.
- **Pages that were not played.** Nothing was requested, so nothing is captured.
- **Live streams**, in the first stages (6.1, 6.2).
- **Transports other than HTTP(S)**: WebRTC, WebSocket segments, RTSP.
- **Terms of service and copyright.** These are the person's to respect (section 1.3).

## 13. Build order

Each stage ships with tests, and an item is not done until its checks pass. The hand-off record below is a
sketch; its final shape is settled in the pull request for stage 2.

```json
{ "pageUrl": "https://example.com/watch/42", "pageTitle": "…",
  "candidates": [
    { "kind": "file",    "url": "https://cdn.example.net/v/42.mp4", "mime": "video/mp4", "size": 183746221,
      "headers": { "Referer": "…", "User-Agent": "…", "Cookie": "…" },
      "evidence": { "score": 0.97, "hits": ["type:media", "mime:video/mp4", "206:total"] } },
    { "kind": "chunked", "url": "https://cdn.example.net/v?id=42", "range": { "in": "query", "name": "range" },
      "pieceMax": 1048576, "size": 91234567 },
    { "kind": "hls",     "url": "https://cdn.example.net/42/master.m3u8" },
    { "kind": "dash",    "url": "https://cdn.example.net/42/manifest.mpd" } ] }
```

| Stage | What | Passes |
|---|---|---|
| 0 | This document, the check and its mutation test | `C01`–`C20`; CI runs both |
| 1 | Request headers in `START` (ROADMAP item 3): the allow-list, host binding, no logging | The protocol tests on both sides; a behaviour test that a cookie reaches the server and not another host after a redirect |
| 2 | Extension observes and classifies; single files and chunked files are handed to the daemon, which keeps a registry of candidates and starts a download; the popup lists them; the native host carries the token | `C01`–`C05` as the oracle for the classifier, run against recorded sessions replayed from a script; the extension's own tests |
| 3 | HLS: the daemon parses and plans; the core gets an ordered multi-request job with a window, per-segment retry and check, AES-128, and resume at segment granularity | `C06`–`C09`, `C14`–`C16` as the oracle; behaviour tests against a scripted HLS origin (out-of-order completion, a failing segment, a mid-run change, a restart); the sanitizer builds |
| 4 | DASH, combining tracks, the optional remux | `C10`, `C11`; a scripted DASH origin |
| 5 | Refresh and route ordering | `C17`–`C19` |
| 6 | Capture by playback | `C20`; a scripted page |

The numbers each stage needs (`T_req`, `r_c`, `δ` per kind of host) come from the benchmark of ROADMAP item
7. Until then the core's existing connection policy decides how many connections to use, and the formulas
of section 7 decide only piece-size floors and window sizes.

## 14. What has to be measured

Everything above that is a model rather than a theorem has an assumption that a measurement can break.

| Assumption | Measured by |
|---|---|
| Features are independent given the class (4.3); the weights, the prior and the thresholds | Labelled sessions: pages with and without video, with their recorded requests |
| The `1/2` threshold on variability (5.2) | The same sessions: how often identity, position and unresolved parameters are told apart correctly |
| `δ`, `T_req` and `r_c` per kind of host; the extra start-up of a connection is serial (7.2) | The benchmark of ROADMAP item 7, in a network namespace with controlled delay and loss |
| Servers accept pieces larger than the player asked for (5.4) | A probe at capture time, per host; recorded |
| Success probabilities of the routes (9) | Counted per host as FluxDM runs |
| Lifetime of signed addresses, and whether a manifest can be fetched again (8) | Recorded at capture time |
| A Manifest V3 `webRequest` listener that asks for `extraHeaders` sees `Cookie` and `Referer`, and does not see `Authorization` (3) | A real browser, before the extension is built on it |
| Whether to depend on `ffmpeg`, optionally, for combining tracks (6.3) | A decision, to be confirmed |

## 15. References

- Internet Download Manager, FAQ:
  [extension permissions](https://www.internetdownloadmanager.com/register/new_faq/chrome_perms.html),
  [video panel missing](https://data.internetdownloadmanager.com/register/new_faq/video1.html),
  [video panel for some videos](https://internetdownloadmanager.com/register/new_faq/video11.html).
- Chrome extensions, [`chrome.webRequest`](https://developer.chrome.com/docs/extensions/reference/api/webRequest).
- R. Pantos, W. May, *HTTP Live Streaming*, [RFC 8216](https://www.rfc-editor.org/rfc/rfc8216): §3 (media
  sequence numbers), §4.3.2.2 (`EXT-X-BYTERANGE`), §4.3.2.4 (`EXT-X-KEY`), §4.3.2.5 (`EXT-X-MAP`),
  §4.3.3.1 (`EXT-X-TARGETDURATION`), §4.3.3.2 (`EXT-X-MEDIA-SEQUENCE`), §4.3.4.2 (`BANDWIDTH`), §5.2 (the
  default IV).
- ISO/IEC 23009-1, *Dynamic adaptive streaming over HTTP (DASH)*: segment templates, timelines and
  `sidx`-indexed ranges.
- W3C, [Media Source Extensions](https://www.w3.org/TR/media-source-2/) and the
  [ISO BMFF byte stream format](https://www.w3.org/TR/mse-byte-stream-format-isobmff/).
- R. Fielding, M. Nottingham, J. Reschke, *HTTP Semantics*, [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110),
  §14 (range requests, `Content-Range`).
- R. L. Graham, "Bounds for certain multiprocessing anomalies", *Bell System Technical Journal* 45 (1966);
  "Bounds on multiprocessing timing anomalies", *SIAM Journal on Applied Mathematics* 17 (1969): list
  scheduling and longest-first.
