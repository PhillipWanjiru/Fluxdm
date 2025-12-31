/* FluxDM — engine/include/SegmentManager.hpp
 * Phase 2: Intelligent segment splitting, stall detection, and dynamic
 * rebalancing. Wraps the C core's raw segment machinery with smarter logic.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <mutex>
#include <atomic>

/* Forward declarations — avoids pulling in C headers from C++ headers */
struct flux_download;
struct flux_segment;

namespace fluxdm {

/* ── Segment descriptor (C++ view, mirrors flux_segment_t state) ─────────── */
struct SegDesc {
    uint8_t  index;
    uint64_t range_start;
    uint64_t range_end;
    uint64_t written;
    float    speed_bps;     /* rolling average bytes/sec  */
    uint64_t last_active;   /* monotonic ms               */
    enum class State { Pending, Connecting, Active, Paused, Complete, Error, Stalled };
    State    state { State::Pending };
};

/* ── Split strategy ──────────────────────────────────────────────────────── */
enum class SplitStrategy {
    Equal,          /* uniform chunk sizes                         */
    SizeWeighted,   /* larger chunks to faster segments            */
    PipelineFirst,  /* start of file gets more segments (warm CDN) */
};

/* ── SegmentManager ──────────────────────────────────────────────────────── */
class SegmentManager {
public:
    /* Tuning constants */
    static constexpr uint32_t STALL_THRESHOLD_MS  = 8'000;   /* 8 s with no bytes  */
    static constexpr uint32_t MIN_STEAL_BYTES      = 512'000; /* don't steal < 512 KB */
    static constexpr uint8_t  MAX_SEGMENTS         = 64;
    static constexpr uint8_t  DEFAULT_SEGMENTS     = 8;

    explicit SegmentManager(uint8_t max_segs = DEFAULT_SEGMENTS,
                            SplitStrategy   strategy = SplitStrategy::Equal);
    ~SegmentManager();

    /* ── Initialise ────────────────────────────────────────────────────────── */

    /* Plan segments for a download. Returns the SegDesc vector. */
    std::vector<SegDesc> plan(uint64_t total_size, uint8_t num_segs,
                              bool accept_ranges) const;

    /* Attach to a running download (pointers into C core). */
    void attach(flux_download *dl);
    void detach();

    /* ── Runtime ───────────────────────────────────────────────────────────── */

    /* Sync local SegDesc view from the C core. Call periodically. */
    void sync();

    /* Detect stalled segments and re-assign their remaining bytes
     * to a faster peer. Returns number of rebalances performed. */
    int  rebalance();

    /* Split one segment into two (used when a new slot becomes available). */
    bool split(uint8_t seg_idx);

    /* Merge two incomplete adjacent segments into one. */
    bool merge(uint8_t a, uint8_t b);

    /* ── Queries ───────────────────────────────────────────────────────────── */

    uint64_t total_downloaded() const;
    uint64_t total_size()       const { return total_size_; }
    float    overall_speed()    const;   /* bytes/sec across all segments */
    uint32_t eta_seconds()      const;
    uint8_t  active_count()     const;
    bool     all_complete()     const;

    const std::vector<SegDesc>& segments() const { return segs_; }

    /* Callback fired when a segment changes state */
    using OnStateChange = std::function<void(const SegDesc&)>;
    void set_on_state_change(OnStateChange cb) { on_state_change_ = std::move(cb); }

private:
    uint8_t        max_segs_;
    SplitStrategy  strategy_;
    uint64_t       total_size_  { 0 };
    flux_download *dl_          { nullptr };

    mutable std::mutex   mu_;
    std::vector<SegDesc> segs_;
    OnStateChange        on_state_change_;

    uint64_t now_ms() const;
    void     update_state(SegDesc& sd, SegDesc::State s);
    bool     is_stalled(const SegDesc& sd, uint64_t now) const;
};

} /* namespace fluxdm */
