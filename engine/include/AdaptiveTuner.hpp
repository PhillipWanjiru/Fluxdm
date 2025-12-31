/* FluxDM — engine/include/AdaptiveTuner.hpp
 * Phase 2: Per-host speed history and optimal segment count suggestion.
 * Learns CDN behaviour over time and auto-tunes connections.
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>

namespace fluxdm {

/* ── Speed sample ────────────────────────────────────────────────────────── */
struct SpeedSample {
    uint64_t timestamp_ms;   /* monotonic */
    float    bytes_per_sec;
    uint8_t  segments_used;
};

/* ── Per-host history ────────────────────────────────────────────────────── */
struct HostProfile {
    std::string host;
    std::deque<SpeedSample> samples;       /* rolling window        */
    float   peak_bps          { 0.f };
    float   avg_bps           { 0.f };
    uint8_t best_seg_count    { 4 };
    uint8_t throttle_hints    { 0 };       /* times we were throttled */
    bool    supports_ranges   { true };
    bool    supports_http2    { false };
    uint64_t last_seen_ms     { 0 };
};

/* ── AdaptiveTuner ───────────────────────────────────────────────────────── */
class AdaptiveTuner {
public:
    static constexpr size_t  SAMPLE_WINDOW      = 20;    /* samples kept per host  */
    static constexpr uint8_t MIN_SEGMENTS        = 2;
    static constexpr uint8_t MAX_SEGMENTS        = 32;
    static constexpr uint8_t DEFAULT_SEGMENTS    = 8;
    static constexpr float   THROTTLE_SPEED_DROP = 0.3f; /* 30% drop = throttle hint */

    AdaptiveTuner() = default;

    /* Record a speed observation for a host */
    void record(const std::string& host, float bytes_per_sec,
                uint8_t segments_used);

    /* Record a throttle event (server rate-limited us) */
    void record_throttle(const std::string& host);

    /* Suggest optimal segment count based on history */
    uint8_t suggest_segments(const std::string& host,
                              uint8_t current_segs) const;

    /* Called when we detect HTTP/2 or range support */
    void set_http2(const std::string& host, bool supported);
    void set_range_support(const std::string& host, bool supported);

    /* Get a copy of a host's profile (for stats/UI) */
    bool get_profile(const std::string& host, HostProfile& out) const;

    /* Forget old hosts (call periodically) */
    void prune(uint64_t max_age_ms = 3'600'000ULL); /* 1 hour default */

    /* Number of known hosts */
    size_t host_count() const;

private:
    mutable std::mutex                          mu_;
    std::unordered_map<std::string, HostProfile> profiles_;

    uint64_t now_ms() const;
    void     recompute_avg(HostProfile& p);
};

} /* namespace fluxdm */
