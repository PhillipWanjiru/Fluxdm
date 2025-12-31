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
