/* FluxDM — engine/src/SegmentManager.cpp */

#include "../include/SegmentManager.hpp"

extern "C" {
#include "../../core/include/segment.h"
#include "../../core/include/downloader.h"
#include "../../core/include/errors.h"
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <numeric>

namespace fluxdm {

/* ── Helpers ─────────────────────────────────────────────────────────────── */

uint64_t SegmentManager::now_ms() const {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool SegmentManager::is_stalled(const SegDesc& sd, uint64_t now) const {
    if (sd.state != SegDesc::State::Active) return false;
    return (now - sd.last_active) > STALL_THRESHOLD_MS;
}

void SegmentManager::update_state(SegDesc& sd, SegDesc::State s) {
    if (sd.state == s) return;
    sd.state = s;
    if (on_state_change_) on_state_change_(sd);
}

/* ── Constructor / destructor ────────────────────────────────────────────── */

SegmentManager::SegmentManager(uint8_t max_segs, SplitStrategy strategy)
    : max_segs_(max_segs), strategy_(strategy) {}

SegmentManager::~SegmentManager() { detach(); }

/* ── Plan ─────────────────────────────────────────────────────────────────── */

std::vector<SegDesc> SegmentManager::plan(uint64_t total_size,
                                           uint8_t  num_segs,
                                           bool     accept_ranges) const {
    std::vector<SegDesc> result;

    /* Cap to hardware limit */
    num_segs = std::min(num_segs, MAX_SEGMENTS);
    if (num_segs == 0) num_segs = 1;

    if (!accept_ranges || total_size == 0 || num_segs == 1) {
        /* Single open-ended segment */
        SegDesc sd;
        sd.index       = 0;
        sd.range_start = 0;
        sd.range_end   = UINT64_MAX;
        sd.written     = 0;
        sd.state       = SegDesc::State::Pending;
        result.push_back(sd);
        return result;
    }

    /* Choose chunk sizes according to strategy */
    std::vector<uint64_t> sizes(num_segs);
    switch (strategy_) {
    case SplitStrategy::Equal: {
        uint64_t chunk = total_size / num_segs;
        uint64_t rem   = total_size % num_segs;
        for (uint8_t i = 0; i < num_segs; ++i)
            sizes[i] = chunk + (i == num_segs - 1 ? rem : 0);
        break;
    }
    case SplitStrategy::SizeWeighted: {
        /* Larger segments at the start (CDN is usually fastest early on) */
        double   total_weight = 0;
        std::vector<double> weights(num_segs);
        for (uint8_t i = 0; i < num_segs; ++i) {
            weights[i]    = static_cast<double>(num_segs - i);
            total_weight += weights[i];
        }
        uint64_t assigned = 0;
        for (uint8_t i = 0; i < num_segs; ++i) {
            sizes[i]  = static_cast<uint64_t>(
                total_size * weights[i] / total_weight);
            assigned += sizes[i];
        }
        /* Fix rounding on last segment */
        sizes[num_segs - 1] += total_size - assigned;
        break;
    }
    case SplitStrategy::PipelineFirst: {
        /* First half of segments cover 70% of the file */
        uint8_t  half    = num_segs / 2;
        uint64_t front   = static_cast<uint64_t>(total_size * 0.7);
        uint64_t back    = total_size - front;
        uint64_t fc      = half > 0 ? front / half : 0;
        uint64_t bc      = (num_segs - half) > 0 ? back / (num_segs - half) : 0;
        for (uint8_t i = 0; i < num_segs; ++i)
            sizes[i] = i < half ? fc : bc;
        /* Correct rounding */
        uint64_t tot = std::accumulate(sizes.begin(), sizes.end(), uint64_t(0));
        sizes[num_segs - 1] += total_size - tot;
        break;
    }
    }

    /* Build descriptors */
    uint64_t offset = 0;
    for (uint8_t i = 0; i < num_segs; ++i) {
        SegDesc sd;
        sd.index       = i;
        sd.range_start = offset;
        sd.range_end   = offset + sizes[i] - 1;
        sd.written     = 0;
        sd.state       = SegDesc::State::Pending;
        result.push_back(sd);
        offset += sizes[i];
    }

    return result;
}

/* ── Attach / detach ─────────────────────────────────────────────────────── */

void SegmentManager::attach(flux_download *dl) {
    std::lock_guard<std::mutex> lock(mu_);
    dl_         = dl;
    total_size_ = dl ? dl->total_size : 0;

    if (!dl) return;

    /* Mirror existing segments */
    segs_.clear();
    for (uint8_t i = 0; i < dl->num_segs; ++i) {
        flux_segment_t *seg = dl->segs[i];
        if (!seg) continue;
        SegDesc sd;
        sd.index       = i;
        sd.range_start = seg->range_start;
        sd.range_end   = seg->range_end;
        sd.written     = seg->written;
        sd.speed_bps   = static_cast<float>(seg->speed_bps);
        sd.last_active = seg->last_active_ms;
        switch (seg->state) {
        case SEG_PENDING:    sd.state = SegDesc::State::Pending;     break;
        case SEG_BODY:       sd.state = SegDesc::State::Active;      break;
        case SEG_PAUSED:     sd.state = SegDesc::State::Paused;      break;
        case SEG_COMPLETE:   sd.state = SegDesc::State::Complete;    break;
        case SEG_ERROR:      sd.state = SegDesc::State::Error;       break;
        default:             sd.state = SegDesc::State::Connecting;  break;
        }
        segs_.push_back(sd);
    }
}

void SegmentManager::detach() {
    std::lock_guard<std::mutex> lock(mu_);
    dl_ = nullptr;
}

/* ── Sync ────────────────────────────────────────────────────────────────── */

void SegmentManager::sync() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dl_) return;

    uint64_t now = now_ms();
    for (uint8_t i = 0; i < dl_->num_segs && i < segs_.size(); ++i) {
        flux_segment_t *seg = dl_->segs[i];
        if (!seg) continue;

        SegDesc& sd    = segs_[i];
        sd.written     = seg->written;
        sd.speed_bps   = static_cast<float>(seg->speed_bps);
        sd.last_active = seg->last_active_ms;

        /* Detect stall */
        if (sd.state == SegDesc::State::Active && is_stalled(sd, now))
            update_state(sd, SegDesc::State::Stalled);
    }
}

/* ── Rebalance ────────────────────────────────────────────────────────────── */

int SegmentManager::rebalance() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dl_ || segs_.size() < 2) return 0;

    uint64_t now      = now_ms();
    int      count    = 0;

    for (auto& slow : segs_) {
        if (!is_stalled(slow, now)) continue;

        uint64_t slow_remaining = slow.range_end - slow.range_start - slow.written + 1;
        if (slow.range_end == UINT64_MAX || slow_remaining < MIN_STEAL_BYTES) continue;

        /* Find the fastest active segment with available capacity */
        SegDesc *best = nullptr;
        for (auto& fast : segs_) {
            if (fast.index == slow.index) continue;
            if (fast.state != SegDesc::State::Active) continue;
            if (!best || fast.speed_bps > best->speed_bps) best = &fast;
        }
        if (!best) continue;

        /* Steal the back half of the stalled segment's remaining range */
        uint64_t steal_start = slow.range_start + slow.written
                               + slow_remaining / 2;
        uint64_t steal_end   = slow.range_end;

        /* Shrink the stalled segment */
        slow.range_end = steal_start - 1;
        if (dl_->segs[slow.index])
            dl_->segs[slow.index]->range_end = steal_start - 1;

        /* Spawn a new segment for the stolen range if slots available */
        if (dl_->num_segs < max_segs_) {
            uint8_t new_idx = dl_->num_segs;
            flux_segment_t *new_seg = flux_seg_new(dl_, new_idx,
                                                    steal_start, steal_end);
            if (new_seg) {
                new_seg->file_fd      = dl_->file_fd;
                dl_->segs[new_idx]    = new_seg;
                dl_->num_segs++;

                SegDesc nd;
                nd.index       = new_idx;
                nd.range_start = steal_start;
                nd.range_end   = steal_end;
                nd.state       = SegDesc::State::Pending;
                segs_.push_back(nd);

                flux_seg_start(new_seg, dl_->epoll_fd);
                dl_->num_segs_active++;
                count++;
            }
        }
    }
    return count;
}

/* ── Split / merge ────────────────────────────────────────────────────────── */

bool SegmentManager::split(uint8_t seg_idx) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dl_ || dl_->num_segs >= max_segs_) return false;
    if (seg_idx >= segs_.size()) return false;

    SegDesc& sd = segs_[seg_idx];
    if (sd.range_end == UINT64_MAX) return false;   /* open-ended: can't split */

    uint64_t remaining  = sd.range_end - sd.range_start - sd.written;
    if (remaining < MIN_STEAL_BYTES * 2) return false;

    uint64_t mid        = sd.range_start + sd.written + remaining / 2;
    uint64_t old_end    = sd.range_end;

    /* Shrink existing */
    sd.range_end = mid - 1;
    if (dl_->segs[seg_idx]) dl_->segs[seg_idx]->range_end = mid - 1;

    /* Create new */
    uint8_t new_idx = dl_->num_segs;
    flux_segment_t *new_seg = flux_seg_new(dl_, new_idx, mid, old_end);
    if (!new_seg) { sd.range_end = old_end; return false; }

    new_seg->file_fd   = dl_->file_fd;
    dl_->segs[new_idx] = new_seg;
    dl_->num_segs++;

    SegDesc nd;
    nd.index       = new_idx;
    nd.range_start = mid;
    nd.range_end   = old_end;
    nd.state       = SegDesc::State::Pending;
    segs_.push_back(nd);

    flux_seg_start(new_seg, dl_->epoll_fd);
    dl_->num_segs_active++;
    return true;
}

bool SegmentManager::merge(uint8_t a, uint8_t b) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dl_ || a >= segs_.size() || b >= segs_.size()) return false;

    SegDesc& sa = segs_[a];
    SegDesc& sb = segs_[b];
    if (sa.state == SegDesc::State::Active ||
        sb.state == SegDesc::State::Active) return false;
    if (sa.range_end + 1 != sb.range_start) return false; /* must be adjacent */

    sa.range_end = sb.range_end;
    if (dl_->segs[a]) dl_->segs[a]->range_end = sb.range_end;

    /* Abort the second segment */
    if (dl_->segs[b]) { flux_seg_abort(dl_->segs[b]); flux_seg_free(dl_->segs[b]); }
    dl_->segs[b] = nullptr;

    update_state(sb, SegDesc::State::Complete);  /* mark as absorbed */
    return true;
}

/* ── Queries ─────────────────────────────────────────────────────────────── */

uint64_t SegmentManager::total_downloaded() const {
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t total = 0;
    for (const auto& sd : segs_) total += sd.written;
    return total;
}

float SegmentManager::overall_speed() const {
    std::lock_guard<std::mutex> lock(mu_);
    float total = 0.f;
    for (const auto& sd : segs_)
        if (sd.state == SegDesc::State::Active) total += sd.speed_bps;
    return total;
}

uint32_t SegmentManager::eta_seconds() const {
    float speed = overall_speed();
    if (speed <= 0.f || total_size_ == 0) return UINT32_MAX;
    uint64_t done = total_downloaded();
    if (done >= total_size_) return 0;
    return static_cast<uint32_t>(static_cast<double>(total_size_ - done) / speed);
}

uint8_t SegmentManager::active_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    uint8_t n = 0;
    for (const auto& sd : segs_)
        if (sd.state == SegDesc::State::Active) ++n;
    return n;
}

bool SegmentManager::all_complete() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::all_of(segs_.begin(), segs_.end(),
        [](const SegDesc& sd){ return sd.state == SegDesc::State::Complete; });
}

} /* namespace fluxdm */
