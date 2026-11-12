/* FluxDM — tests/test_engine.cpp
 * Phase 2 — C++ Engine Layer unit tests.
 * Self-contained, no external test framework.
 */

#include "../engine/include/SegmentManager.hpp"
#include "../engine/include/AdaptiveTuner.hpp"
#include "../engine/include/BufferManager.hpp"
#include "../engine/include/MetadataParser.hpp"
#include "../engine/include/TorrentEngine.hpp"
#include "../engine/include/PeerDiscovery.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>

using namespace fluxdm;

static int g_pass = 0, g_fail = 0;

#define RESET  "\033[0m"
#define GREEN  "\033[92m"
#define RED    "\033[91m"
#define CYAN   "\033[96m"
#define BOLD   "\033[1m"

#define SUITE(name) std::cout << "\n" BOLD CYAN "── " name RESET "\n"

#define EXPECT(cond) do { \
    if (cond) { std::cout << "  " GREEN "✔" RESET "  " #cond "\n"; g_pass++; } \
    else { std::cout << "  " RED "✘" RESET "  " #cond "  (" __FILE__ ":" \
                     << __LINE__ << ")\n"; g_fail++; } \
} while(0)

#define EXPECT_EQ(a,b) do { \
    auto _a=(a); auto _b=(b); \
    if (_a==_b) { std::cout << "  " GREEN "✔" RESET "  " #a " == " #b "\n"; g_pass++; } \
    else { std::cout << "  " RED "✘" RESET "  " #a " == " #b \
                     << "  (got " << _a << ", expected " << _b << ")\n"; g_fail++; } \
} while(0)

/* ── SegmentManager ──────────────────────────────────────────────────────── */
static void test_segment_manager() {
    SUITE("SegmentManager");

    SegmentManager mgr(4, SplitStrategy::Equal);

    /* Plan: 1MB file, 4 segments, ranges supported */
    auto plan = mgr.plan(1024*1024, 4, true);
    EXPECT_EQ(plan.size(), size_t(4));
    EXPECT_EQ(plan[0].range_start, uint64_t(0));
    EXPECT_EQ(plan[3].range_end,   uint64_t(1024*1024-1));

    /* Ranges must be contiguous with no gaps */
    bool contiguous = true;
    for (size_t i = 1; i < plan.size(); ++i)
        if (plan[i].range_start != plan[i-1].range_end + 1) contiguous = false;
    EXPECT(contiguous);

    /* Plan: no range support → single segment */
    auto plan2 = mgr.plan(1024*1024, 8, false);
    EXPECT_EQ(plan2.size(), size_t(1));
    EXPECT_EQ(plan2[0].range_end, UINT64_MAX);

    /* Plan: unknown size → single segment */
    auto plan3 = mgr.plan(0, 8, true);
    EXPECT_EQ(plan3.size(), size_t(1));

    /* SizeWeighted strategy */
    SegmentManager mgr2(4, SplitStrategy::SizeWeighted);
    auto plan4 = mgr2.plan(1000000, 4, true);
    EXPECT_EQ(plan4.size(), size_t(4));
    /* First segment should be larger than last (weighted toward front) */
    uint64_t first_size = plan4[0].range_end - plan4[0].range_start + 1;
    uint64_t last_size  = plan4[3].range_end - plan4[3].range_start + 1;
    EXPECT(first_size > last_size);

    /* No segments attached yet */
    EXPECT_EQ(mgr.active_count(), uint8_t(0));
    EXPECT(mgr.all_complete());  /* vacuously true, no segments attached */
}

/* ── AdaptiveTuner ───────────────────────────────────────────────────────── */
static void test_adaptive_tuner() {
    SUITE("AdaptiveTuner");

    AdaptiveTuner tuner;

    /* Unknown host → default suggestion */
    EXPECT_EQ(tuner.suggest_segments("unknown.host", 8),
              AdaptiveTuner::DEFAULT_SEGMENTS);

    /* Record samples for a fast host */
    for (int i = 0; i < 10; ++i)
        tuner.record("fast.cdn.com", 10.0f * 1024*1024, 8); /* 10 MB/s @ 8 segs */

    HostProfile p;
    EXPECT(tuner.get_profile("fast.cdn.com", p));
    EXPECT(p.samples.size() == 10);
    EXPECT(p.avg_bps > 0.f);

    /* Throttle detection: sudden speed drop */
    tuner.record("throttled.host", 10.0f*1024*1024, 8);
    tuner.record("throttled.host", 10.0f*1024*1024, 8);
    tuner.record("throttled.host", 10.0f*1024*1024, 8);
    tuner.record("throttled.host", 2.0f*1024*1024, 8);  /* big drop */
    tuner.record_throttle("throttled.host");
    tuner.record_throttle("throttled.host");
    tuner.record_throttle("throttled.host");

    HostProfile tp;
    tuner.get_profile("throttled.host", tp);
    EXPECT(tp.throttle_hints >= 3);

    /* Suggestion should back off when throttled */
    uint8_t suggestion = tuner.suggest_segments("throttled.host", 16);
    EXPECT(suggestion < 16);

    /* No range support → 1 segment */
    tuner.set_range_support("no-ranges.host", false);
    EXPECT_EQ(tuner.suggest_segments("no-ranges.host", 8), uint8_t(1));

    /* HTTP/2 host → capped at 4 */
    tuner.set_http2("h2.host", true);
    for (int i = 0; i < 5; ++i) tuner.record("h2.host", 5.0f*1024*1024, 8);
    uint8_t h2_suggestion = tuner.suggest_segments("h2.host", 8);
    EXPECT(h2_suggestion <= 4);

    EXPECT(tuner.host_count() >= size_t(4));
}

/* ── BufferManager ───────────────────────────────────────────────────────── */
static void test_buffer_manager() {
    SUITE("BufferManager");

    BufferManager bm(8, 64*1024);  /* 8 slots of 64KB */

    EXPECT_EQ(bm.pool_size(), size_t(8));
    EXPECT_EQ(bm.slots_in_use(), size_t(0));

    /* Acquire and release */
    BufferSlot *slot = bm.acquire();
    EXPECT(slot != nullptr);
    EXPECT_EQ(bm.slots_in_use(), size_t(1));
    bm.release(slot);
    EXPECT_EQ(bm.slots_in_use(), size_t(0));

    /* Write coalescing to a temp file */
    char tmpname[] = "/tmp/fluxdm_buftest_XXXXXX";
    int fd = mkstemp(tmpname);
    EXPECT(fd >= 0);

    const char data1[] = "Hello, ";
    const char data2[] = "FluxDM!";

    bool flushed1 = bm.enqueue(fd, 0, data1, strlen(data1));
    bool flushed2 = bm.enqueue(fd, strlen(data1), data2, strlen(data2));

    /* Adjacent writes should coalesce (not immediately flush) */
    EXPECT(!flushed1);
    EXPECT(!flushed2);   /* coalesced into the same pending buffer */

    bm.flush(fd);
    EXPECT_EQ(bm.pending_writes(), size_t(0));

    /* Verify file content */
    char buf[32] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    EXPECT(n > 0);
    std::string expected = std::string(data1) + data2;
    EXPECT(std::string(buf) == expected);

    close(fd);
    unlink(tmpname);

    EXPECT(bm.total_flushed() >= strlen(data1) + strlen(data2));
}

/* ── MetadataParser ──────────────────────────────────────────────────────── */
static void test_metadata_parser() {
    SUITE("MetadataParser");

    /* URL decode */
    EXPECT(MetadataParser::url_decode("hello%20world") == "hello world");
    EXPECT(MetadataParser::url_decode("a+b+c") == "a b c");

    /* Filename sanitisation: '/' → '_', then leading dots stripped
     * (prevents the sanitised name from becoming a hidden dotfile) */
    EXPECT(MetadataParser::sanitise_filename("../../etc/passwd") == "_.._etc_passwd");
    EXPECT(MetadataParser::sanitise_filename("normal_file.txt") == "normal_file.txt");
    EXPECT(MetadataParser::sanitise_filename("") == "download");

    /* Filename from URL */
    EXPECT(MetadataParser::filename_from_url(
        "https://example.com/path/to/file.zip") == "file.zip");
    EXPECT(MetadataParser::filename_from_url(
        "https://example.com/file.zip?token=abc") == "file.zip");
    EXPECT(MetadataParser::filename_from_url(
        "https://example.com/path%20with%20spaces/doc.pdf") == "doc.pdf");

    /* MIME → extension */
    EXPECT(MetadataParser::mime_to_extension("video/mp4") == ".mp4");
    EXPECT(MetadataParser::mime_to_extension("application/pdf") == ".pdf");
    EXPECT(MetadataParser::mime_to_extension("video/mp4; codecs=avc1") == ".mp4");
    EXPECT(MetadataParser::mime_to_extension("unknown/type") == "");

    /* Content-Disposition parsing */
    EXPECT(MetadataParser::parse_content_disposition(
        "attachment; filename=\"report.pdf\"") == "report.pdf");
    EXPECT(MetadataParser::parse_content_disposition(
        "attachment; filename*=UTF-8''na%C3%AFve.txt") == "na\xC3\xAFve.txt");

    /* Full header block parse */
    MetadataParser parser;
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 52428800\r\n"
        "Content-Type: video/mp4\r\n"
        "Accept-Ranges: bytes\r\n"
        "ETag: \"abc123\"\r\n"
        "Content-Disposition: attachment; filename=\"movie.mp4\"\r\n"
        "\r\n";

    DownloadMeta meta = parser.parse(headers, "https://cdn.example.com/video");
    EXPECT_EQ(meta.content_length, uint64_t(52428800));
    EXPECT(meta.accept_ranges);
    EXPECT(meta.mime_type == "video/mp4");
    EXPECT(meta.filename == "movie.mp4");
    EXPECT(meta.etag == "\"abc123\"");
    EXPECT(meta.is_attachment);

    /* Conditional headers */
    std::string cond = MetadataParser::make_conditional_headers(meta);
    EXPECT(cond.find("If-None-Match") != std::string::npos);
}

/* ── TorrentEngine — bencode ─────────────────────────────────────────────── */
static void test_bencode() {
    SUITE("TorrentEngine — Bencode");

    /* Integer */
    auto v1 = TorrentEngine::bencode_parse("i42e");
    EXPECT(v1 != nullptr);
    EXPECT_EQ(v1->int_val, int64_t(42));

    /* Negative integer */
    auto v2 = TorrentEngine::bencode_parse("i-7e");
    EXPECT_EQ(v2->int_val, int64_t(-7));

    /* String */
    auto v3 = TorrentEngine::bencode_parse("4:spam");
    EXPECT(v3->str_val == "spam");

    /* List */
    auto v4 = TorrentEngine::bencode_parse("l4:spam4:eggse");
    EXPECT_EQ(v4->list_val.size(), size_t(2));
    EXPECT(v4->list_val[0]->str_val == "spam");
    EXPECT(v4->list_val[1]->str_val == "eggs");

    /* Dict */
    auto v5 = TorrentEngine::bencode_parse("d3:cow3:moo4:spam4:eggse");
    EXPECT_EQ(v5->dict_val.size(), size_t(2));
    EXPECT(v5->dict_val["cow"]->str_val  == "moo");
    EXPECT(v5->dict_val["spam"]->str_val == "eggs");

    /* Nested */
    auto v6 = TorrentEngine::bencode_parse("d4:listl1:a1:bee");
    EXPECT(v6->dict_val["list"]->list_val.size() == size_t(2));
}

/* ── TorrentEngine — Magnet URI ──────────────────────────────────────────── */
static void test_magnet() {
    SUITE("TorrentEngine — Magnet URI");

    std::string uri =
        "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a"
        "&dn=Test+File"
        "&tr=http%3A%2F%2Ftracker.example.com%2Fannounce"
        "&tr=udp%3A%2F%2Ftracker2.example.com%3A6969";

    MagnetURI m = TorrentEngine::parse_magnet(uri);
    EXPECT(m.is_valid());
    EXPECT(m.info_hash_hex == "c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    EXPECT(m.display_name == "Test File");
    EXPECT_EQ(m.trackers.size(), size_t(2));
    EXPECT(m.trackers[0] == "http://tracker.example.com/announce");

    /* Invalid magnet */
    MagnetURI bad = TorrentEngine::parse_magnet("not-a-magnet");
    EXPECT(!bad.is_valid());
}

/* ── TorrentEngine — SHA1 ────────────────────────────────────────────────── */
static void test_sha1() {
    SUITE("TorrentEngine — SHA-1");

    /* Known SHA-1 of "abc" = a9993e364706816aba3e25717850c26c9cd0d89d
     * (verified via `echo -n "abc" | sha1sum`) */
    const char *input = "abc";
    std::string hex = TorrentEngine::sha1_hex(
        reinterpret_cast<const uint8_t*>(input), 3);
    EXPECT(hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

/* ── Untrusted input: .torrent files, bencode, file names ────────────────────
 * Everything a torrent or a header block says is written by whoever runs the server. These tests are the regressions
 * for what fuzzing under ASan/UBSan and valgrind found in the engine (docs/AUDIT.md, Phase 2). */
static bool bencode_throws(const std::string& text) {
    try { (void)TorrentEngine::bencode_parse(text); } catch (const std::exception&) { return true; }
    return false;
}

static void test_untrusted_input() {
    SUITE("Untrusted input — bencode limits, base32, file names, .torrent validation");

    /* nesting is bounded: this used to overflow the stack (a torrent, a tracker reply or a DHT packet of 100 KB) */
    EXPECT(!bencode_throws(std::string(60, 'l') + std::string(60, 'e')));
    EXPECT(bencode_throws(std::string(100000, 'l')));
    EXPECT(bencode_throws(std::string(100000, 'd')));
    /* a length that wraps when added to the position used to pass the "is it all there" check */
    EXPECT(bencode_throws("18446744073709551615:abc"));
    EXPECT(bencode_throws("9223372036854775807:abc"));
    EXPECT(bencode_throws("99999999999999999999:abc"));
    /* numbers are digits, with an optional minus: no sign, no spaces, no trailing text */
    EXPECT(bencode_throws("i5xe"));
    EXPECT(bencode_throws("i+5e"));
    EXPECT(bencode_throws("i 5e"));
    EXPECT(bencode_throws("ie"));
    EXPECT(bencode_throws("i99999999999999999999e"));
    EXPECT(bencode_throws("3x:abc"));
    EXPECT(TorrentEngine::bencode_parse("i-5e")->int_val == -5);
    EXPECT(TorrentEngine::bencode_parse("3:abc")->str_val == "abc");

    /* base32: the accumulator was a signed int that overflowed after six characters (undefined behaviour) */
    EXPECT(TorrentEngine::base32_decode("JBSWY3DPEHPK3PXP") == std::string("Hello!\xDE\xAD\xBE\xEF"));
    EXPECT_EQ(TorrentEngine::base32_decode(std::string(4000, 'A')).size(), size_t(2500));

    /* file names: control characters and the cut in the middle of a UTF-8 sequence */
    EXPECT(MetadataParser::sanitise_filename("a\nb\x1b[31m\x7f.txt") == "a_b_[31m_.txt");
    std::string long_name = std::string(254, 'a') + "\xC3\xA9";                  /* 254 bytes + a two-byte letter */
    EXPECT(MetadataParser::sanitise_filename(long_name) == std::string(254, 'a'));
    EXPECT(MetadataParser::sanitise_filename("..") == "download");
    EXPECT(MetadataParser::sanitise_filename("../../etc/passwd") == "_.._etc_passwd");
    /* a Content-Disposition of 100 KB used to overflow the stack in std::regex */
    EXPECT(MetadataParser::parse_content_disposition("attachment; filename=" + std::string(200000, 'a')).empty());
    EXPECT(MetadataParser::parse_content_disposition("attachment; filename=\"a b.zip\"; size=5") == "a b.zip");
    EXPECT(MetadataParser::parse_content_disposition("attachment; FILENAME = x.bin") == "x.bin");
    EXPECT(MetadataParser::parse_content_disposition("attachment; filename=\"fallback.txt\"; filename*=UTF-8''real%20name.txt") == "real name.txt");
    EXPECT(MetadataParser::parse_content_disposition("inline").empty());

    /* .torrent files */
    auto bstr = [](const std::string& s) { return std::to_string(s.size()) + ":" + s; };
    auto bint = [](int64_t v) { return "i" + std::to_string(v) + "e"; };
    const std::string two_pieces(40, 'X'), three_pieces(60, 'X');
    auto single = [&](const std::string& name, int64_t length, int64_t piece_length, const std::string& pieces) {
        return "d4:infod" + bstr("length") + bint(length) + bstr("name") + bstr(name) + bstr("piece length") + bint(piece_length) +
               bstr("pieces") + bstr(pieces) + "ee";
    };
    auto multi = [&](const std::vector<std::string>& path) {
        std::string comps;
        for (const auto& c : path) comps += bstr(c);
        return "d4:infod" + bstr("files") + "ld" + bstr("length") + bint(1000) + bstr("path") + "l" + comps + "eee" + bstr("name") + bstr("dir") +
               bstr("piece length") + bint(500) + bstr("pieces") + bstr(two_pieces) + "ee";
    };
    auto loads = [](const std::string& torrent) {
        TorrentEngine e;
        return e.load_bytes(std::vector<uint8_t>(torrent.begin(), torrent.end()));
    };

    EXPECT(loads(single("a.bin", 1000, 500, two_pieces)));
    EXPECT(loads(multi({"sub", "x.txt"})));
    /* a torrent must not be able to place a file outside the download directory */
    EXPECT(!loads(multi({"..", "x.txt"})));
    EXPECT(!loads(multi({"sub", ".."})));
    EXPECT(!loads(multi({"/etc/cron.d", "x"})));
    EXPECT(!loads(multi({"a/../../x"})));
    EXPECT(!loads(multi({"."})));
    EXPECT(!loads(multi({""})));
    EXPECT(!loads(multi({})));
    EXPECT(!loads(single("a/b", 1000, 500, two_pieces)));
    EXPECT(!loads(single("..", 1000, 500, two_pieces)));
    EXPECT(!loads(single("a\nb", 1000, 500, two_pieces)));
    /* sizes: a piece length of 0 used to divide by zero later, a negative or huge one wrapped */
    EXPECT(!loads(single("a", 1000, 0, two_pieces)));
    EXPECT(!loads(single("a", 1000, -500, two_pieces)));
    EXPECT(!loads(single("a", 1000, int64_t(1) << 40, two_pieces)));
    EXPECT(!loads(single("a", -1, 500, two_pieces)));
    EXPECT(!loads(single("a", 0, 500, two_pieces)));
    EXPECT(!loads(single("a", int64_t(1) << 61, 500, two_pieces)));
    EXPECT(!loads(single("a", 1000, 500, three_pieces)));              /* the pieces do not match the size */
    EXPECT(!loads(single("a", 1000, 500, std::string(39, 'X'))));      /* not a whole number of hashes */
    EXPECT(!loads(single("a", 1000, 500, "")));

    /* the last piece is what is left of the content (it was the full piece length: the total was not yet known) */
    {
        TorrentEngine e;
        const std::string t = single("a.bin", 1100, 500, three_pieces);
        EXPECT(e.load_bytes(std::vector<uint8_t>(t.begin(), t.end())));
        EXPECT_EQ(e.info().pieces.size(), size_t(3));
        EXPECT_EQ(e.info().pieces[0].length, uint32_t(500));
        EXPECT_EQ(e.info().pieces[2].length, uint32_t(100));
        EXPECT_EQ(e.info().total_size, uint64_t(1100));
    }

    /* the info-hash is the SHA-1 of the info dictionary as written, not of a re-encoding of it: here the keys are not in
     * sorted order, so the two differ. Expected values from Python's hashlib over the same bytes. */
    {
        const std::string unsorted_info = "d" + bstr("name") + bstr("test.bin") + bstr("length") + bint(1000) + bstr("piece length") + bint(500) +
                                          bstr("pieces") + bstr(two_pieces) + "e";
        const std::string canonical_info = "d" + bstr("length") + bint(1000) + bstr("name") + bstr("test.bin") + bstr("piece length") + bint(500) +
                                           bstr("pieces") + bstr(two_pieces) + "e";
        TorrentEngine a, b;
        const std::string ta = "d4:info" + unsorted_info + "e", tb = "d4:info" + canonical_info + "e";
        EXPECT(a.load_bytes(std::vector<uint8_t>(ta.begin(), ta.end())));
        EXPECT(b.load_bytes(std::vector<uint8_t>(tb.begin(), tb.end())));
        EXPECT(a.info().info_hash == "d2aa287922b0cc3e66cdc9b445240321e9320d4f");
        EXPECT(b.info().info_hash == "9aaa88407a64e6c8a7858c10306143bf1183ba16");
        EXPECT_EQ(a.info().info_hash_raw.size(), size_t(20));
    }

    /* a torrent that is refused leaves nothing of itself, or of the one before it, behind */
    {
        TorrentEngine e;
        const std::string good = single("a.bin", 1000, 500, two_pieces), bad = multi({"..", "x"});
        EXPECT(e.load_bytes(std::vector<uint8_t>(good.begin(), good.end())));
        EXPECT(!e.load_bytes(std::vector<uint8_t>(bad.begin(), bad.end())));
        EXPECT(!e.loaded());
        EXPECT(e.info().name.empty() && e.info().files.empty() && e.info().pieces.empty());
    }
    /* nothing in a fresh engine is left uninitialised (valgrind: the last-piece code read total_size before it was set) */
    {
        TorrentEngine e;
        EXPECT_EQ(e.info().piece_length, uint32_t(0));
        EXPECT_EQ(e.info().total_size, uint64_t(0));
        EXPECT(!e.info().is_valid());
    }
}

/* ── TorrentEngine — full .torrent construction ──────────────────────────── */
static void test_torrent_construction() {
    SUITE("TorrentEngine — Synthetic .torrent parse");

    /* Build a minimal valid single-file torrent in bencode.
     * Lengths are computed via bstr() so hand-counting mistakes
     * (like an off-by-one in a URL length prefix) can't happen. */
    auto bstr = [](const std::string& s) {
        return std::to_string(s.size()) + ":" + s;
    };
    auto bint = [](int64_t v) { return "i" + std::to_string(v) + "e"; };

    std::string announce_url = "http://tracker.com/announce";
    std::string piece_hash(20, 'X');  /* dummy 20-byte hash, repeated twice */
    std::string pieces_concat = piece_hash + piece_hash;

    std::string info =
        "d" +
          bstr("length")       + bint(1000) +
          bstr("name")         + bstr("test.bin") +
          bstr("piece length") + bint(500) +
          bstr("pieces")       + bstr(pieces_concat) +
        "e";

    std::string torrent =
        "d" +
          bstr("announce") + bstr(announce_url) +
          bstr("info")     + info +
        "e";

    TorrentEngine engine;
    bool ok = engine.load_bytes(
        std::vector<uint8_t>(torrent.begin(), torrent.end()));
    EXPECT(ok);

    if (ok) {
        const auto& info_parsed = engine.info();
        EXPECT(info_parsed.name == "test.bin");
        EXPECT_EQ(info_parsed.total_size, uint64_t(1000));
        EXPECT_EQ(info_parsed.piece_length, uint32_t(500));
        EXPECT_EQ(info_parsed.pieces.size(), size_t(2));
        EXPECT(!info_parsed.info_hash.empty());
        EXPECT(info_parsed.announce == announce_url);
    }
}

/* ── PeerDiscovery — compact peer parsing ────────────────────────────────── */
static void test_peer_discovery() {
    SUITE("PeerDiscovery");

    PeerDiscovery pd(6881);

    /* Build a compact peer string: 2 peers, 6 bytes each */
    std::string compact;
    compact += static_cast<char>(192); compact += static_cast<char>(168);
    compact += static_cast<char>(1);   compact += static_cast<char>(1);
    compact += static_cast<char>(0x1A); compact += static_cast<char>(0xE1); /* port 6881 */

    compact += static_cast<char>(10);  compact += static_cast<char>(0);
    compact += static_cast<char>(0);   compact += static_cast<char>(5);
    compact += static_cast<char>(0x1A); compact += static_cast<char>(0xE1);

    pd.add_compact_peers(compact);
    EXPECT_EQ(pd.peer_count(), size_t(2));

    auto peers = pd.get_peers(10);
    EXPECT_EQ(peers.size(), size_t(2));
    EXPECT(peers[0].ip == "192.168.1.1");
    EXPECT_EQ(peers[0].port, uint16_t(6881));

    /* Dedup: adding same peers again should not increase count */
    pd.add_compact_peers(compact);
    EXPECT_EQ(pd.peer_count(), size_t(2));

    /* Add tracker */
    pd.add_tracker("http://tracker.example.com/announce");
    EXPECT_EQ(pd.tracker_count(), size_t(1));
    pd.add_tracker("http://tracker.example.com/announce");  /* dup */
    EXPECT_EQ(pd.tracker_count(), size_t(1));               /* still 1 */

    /* PEX round-trip */
    std::string pex = pd.pex_generate();
    EXPECT(pex.size() == 12);  /* 2 peers × 6 bytes */
}

/* ── Summary ──────────────────────────────────────────────────────────────── */
int main() {
    std::cout << "\n" BOLD CYAN " FluxDM Phase 2 — Engine Unit Tests" RESET "\n";

    test_segment_manager();
    test_adaptive_tuner();
    test_buffer_manager();
    test_metadata_parser();
    test_bencode();
    test_magnet();
    test_sha1();
    test_torrent_construction();
    test_untrusted_input();
    test_peer_discovery();

    std::cout << "\n" BOLD CYAN "─────────────────────────────" RESET "\n";
    std::cout << BOLD "  Results" RESET "\n";
    std::cout << CYAN "─────────────────────────────" RESET "\n";
    std::cout << "  " GREEN "✔  Passed : " << g_pass << RESET "\n";
    std::cout << "  " << (g_fail ? RED : "") << "✘  Failed : " << g_fail << RESET "\n";
    std::cout << "     Total  : " << (g_pass + g_fail) << "\n";
    std::cout << CYAN "─────────────────────────────" RESET "\n\n";

    if (g_fail == 0)
        std::cout << BOLD GREEN "ALL TESTS PASSED" RESET "\n\n";
    else
        std::cout << BOLD RED "FAILED" RESET "\n\n";

    return g_fail > 0 ? 1 : 0;
}
