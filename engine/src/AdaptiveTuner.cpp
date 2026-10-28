/* FluxDM — engine/src/AdaptiveTuner.cpp */

#include "../include/AdaptiveTuner.hpp"
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cmath>
#include <vector>

namespace fluxdm {

uint64_t AdaptiveTuner::now_ms() const {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void AdaptiveTuner::recompute_avg(HostProfile& p) {
    if (p.samples.empty()) { p.avg_bps = 0.f; return; }
    float sum = 0.f;
    for (const auto& s : p.samples) sum += s.bytes_per_sec;
    p.avg_bps = sum / static_cast<float>(p.samples.size());

    /* Update peak */
    p.peak_bps = std::max_element(p.samples.begin(), p.samples.end(),
        [](const SpeedSample& a, const SpeedSample& b){
            return a.bytes_per_sec < b.bytes_per_sec;
        })->bytes_per_sec;

    /* Find segment count that correlated with the fastest samples */
    if (p.samples.size() >= 4) {
        /* Sort samples by speed, look at top 25% */
        std::vector<SpeedSample> sorted(p.samples.begin(), p.samples.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const SpeedSample& a, const SpeedSample& b){
                return a.bytes_per_sec > b.bytes_per_sec;
            });
        size_t top_n = std::max(size_t(1), sorted.size() / 4);
        float  best_sum = 0.f;
        std::unordered_map<uint8_t,float> seg_speed;
        for (size_t i = 0; i < top_n; ++i) {
            seg_speed[sorted[i].segments_used] += sorted[i].bytes_per_sec;
        }
        uint8_t best_seg = DEFAULT_SEGMENTS;
        for (const auto& [segs, spd] : seg_speed) {
            if (spd > best_sum) { best_sum = spd; best_seg = segs; }
        }
        p.best_seg_count = best_seg;
    }
}

void AdaptiveTuner::record(const std::string& host,
                            float bytes_per_sec,
                            uint8_t segments_used) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& p = profiles_[host];
    p.host = host;

    SpeedSample s{ now_ms(), bytes_per_sec, segments_used };
    p.samples.push_back(s);
    if (p.samples.size() > SAMPLE_WINDOW)
        p.samples.pop_front();

    p.last_seen_ms = s.timestamp_ms;

    /* Detect throttling: if latest speed is <30% of our peak, hint */
    if (p.peak_bps > 0.f && bytes_per_sec < p.peak_bps * THROTTLE_SPEED_DROP)
        p.throttle_hints = std::min<uint8_t>(p.throttle_hints + 1, 10);
    else if (p.throttle_hints > 0)
        --p.throttle_hints;

    recompute_avg(p);
}

void AdaptiveTuner::record_throttle(const std::string& host) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& p = profiles_[host];
    p.throttle_hints = std::min<uint8_t>(p.throttle_hints + 2, 10);
}

uint8_t AdaptiveTuner::suggest_segments(const std::string& host,
                                         uint8_t current_segs) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = profiles_.find(host);

    /* Unknown host: return default */
    if (it == profiles_.end()) return DEFAULT_SEGMENTS;

    const HostProfile& p = it->second;

    /* If heavily throttled, back off */
    if (p.throttle_hints >= 5) {
        uint8_t backed = static_cast<uint8_t>(current_segs / 2);
        return std::max(backed, MIN_SEGMENTS);
    }

    /* No range support: single segment only */
    if (!p.supports_ranges) return 1;

    /* Not enough data: suggest default */
    if (p.samples.size() < 4) return DEFAULT_SEGMENTS;

    uint8_t suggested = p.best_seg_count;

    /* Clamp to hardware limits */
    suggested = std::clamp(suggested, MIN_SEGMENTS, MAX_SEGMENTS);

    /* Don't change drastically in one step (±2 per call) */
    if (suggested > current_segs + 2) suggested = current_segs + 2;
    if (suggested + 2 < current_segs) suggested = current_segs - 2;

    /* HTTP/2 multiplexes on one connection — fewer segments needed.
     * This is a hard protocol constraint, so it is applied LAST as a
     * final clamp — otherwise the smoothing step above can push the
     * suggestion back above the cap it just enforced. */
    if (p.supports_http2) suggested = std::min(suggested, uint8_t(4));

    return suggested;
}

void AdaptiveTuner::set_http2(const std::string& host, bool supported) {
    std::lock_guard<std::mutex> lock(mu_);
    profiles_[host].supports_http2 = supported;
}

void AdaptiveTuner::set_range_support(const std::string& host, bool supported) {
    std::lock_guard<std::mutex> lock(mu_);
    profiles_[host].supports_ranges = supported;
}

bool AdaptiveTuner::get_profile(const std::string& host, HostProfile& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = profiles_.find(host);
    if (it == profiles_.end()) return false;
    out = it->second;
    return true;
}

void AdaptiveTuner::prune(uint64_t max_age_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t cutoff = now_ms() - max_age_ms;
    for (auto it = profiles_.begin(); it != profiles_.end(); ) {
        if (it->second.last_seen_ms < cutoff)
            it = profiles_.erase(it);
        else
            ++it;
    }
}

size_t AdaptiveTuner::host_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return profiles_.size();
}

} /* namespace fluxdm */
