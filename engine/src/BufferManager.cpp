/* FluxDM — engine/src/BufferManager.cpp */

#include "../include/BufferManager.hpp"
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <atomic>

namespace fluxdm {

/* ── Constructor / destructor ────────────────────────────────────────────── */

BufferManager::BufferManager(size_t pool_size, size_t slot_size)
    : slot_size_(slot_size) {

    pool_.resize(pool_size);
    for (auto& slot : pool_) {
        /* Allocate 4 KB-aligned memory for O_DIRECT compatibility */
        void *mem = nullptr;
        if (posix_memalign(&mem, 4096, slot_size) != 0)
            throw std::bad_alloc();
        slot.mem    = static_cast<uint8_t*>(mem);
        slot.cap    = slot_size;
        slot.len    = 0;
        slot.in_use = false;
    }
}

BufferManager::~BufferManager() {
    flush_all();
    for (auto& slot : pool_) {
        if (slot.mem) { free(slot.mem); slot.mem = nullptr; }
    }
}

/* ── Pool management ─────────────────────────────────────────────────────── */

BufferSlot *BufferManager::acquire() {
    /* Spin briefly before blocking — most acquisitions are immediate */
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(pool_mu_);
            for (auto& slot : pool_) {
                if (!slot.in_use) {
                    slot.in_use     = true;
                    slot.len        = 0;
                    slot.fd         = -1;
                    slot.file_offset= 0;
                    return &slot;
                }
            }
        }
        /* Pool exhausted — yield and retry */
        sched_yield();
    }
    /* Last resort: block */
    std::lock_guard<std::mutex> lock(pool_mu_);
    for (auto& slot : pool_) {
        if (!slot.in_use) {
            slot.in_use = true; slot.len = 0;
            return &slot;
        }
    }
    return nullptr;  /* caller must handle */
}

void BufferManager::release(BufferSlot *slot) {
    if (!slot) return;
    std::lock_guard<std::mutex> lock(pool_mu_);
    slot->in_use = false;
    slot->len    = 0;
}

/* ── Write coalescing ────────────────────────────────────────────────────── */

bool BufferManager::coalesce(WriteRequest& a, const WriteRequest& b) const {
    /* Two writes can be coalesced if b immediately follows a (or small gap) */
    uint64_t a_end = a.file_offset + a.len;
    if (b.file_offset < a_end) return false;               /* overlap */
    if (b.file_offset - a_end > COALESCE_THRESHOLD) return false;  /* too far */
    return true;
}

bool BufferManager::enqueue(int fd, uint64_t file_offset,
                             const void *data, size_t len) {
    std::lock_guard<std::mutex> lock(write_mu_);

    /* Try to coalesce with the last pending write for this fd */
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        if (it->fd != fd) continue;

        uint64_t end = it->file_offset + it->len;
        if (file_offset == end && it->len + len <= slot_size_) {
            /* Perfect adjacency — append to existing request */
            memcpy(it->data + it->len, data, len);
            it->len += len;
            ++syscalls_saved_;
            return false;  /* not yet flushed */
        }
        break;  /* not coalescable */
    }

    /* Acquire a fresh slot */
    BufferSlot *slot = acquire();
    if (!slot) {
        /* No slots — force immediate pwrite (fallback) */
        WriteRequest req{ nullptr, len, file_offset, fd };
        req.data = const_cast<uint8_t*>(static_cast<const uint8_t*>(data));
        do_flush(req);
        return true;
    }

    size_t copy = std::min(len, slot->cap);
    memcpy(slot->mem, data, copy);
    slot->len        = copy;
    slot->file_offset= file_offset;
    slot->fd         = fd;

    pending_.push_back({ slot->mem, copy, file_offset, fd });

    /* Auto-flush if slot is >= 75% full */
    if (slot->len >= slot->cap * 3 / 4) {
        flush(fd);
        return true;
    }
    return false;
}

int BufferManager::do_flush(WriteRequest& req) {
    if (!req.data || req.len == 0) return 0;

    ssize_t n = pwrite(req.fd, req.data, req.len,
                       static_cast<off_t>(req.file_offset));
    if (n < 0) return -errno;

    total_flushed_ += static_cast<size_t>(n);
    if (on_flush_) on_flush_(req.fd, req.file_offset, static_cast<size_t>(n));
    return 0;
}

int BufferManager::flush(int fd) {
    std::lock_guard<std::mutex> lock(write_mu_);
    int rc = 0;

    /* Sort pending writes for this fd by offset (merge adjacents) */
    std::stable_sort(pending_.begin(), pending_.end(),
        [fd](const WriteRequest& a, const WriteRequest& b) {
            if (a.fd != fd) return false;
            if (b.fd != fd) return true;
            return a.file_offset < b.file_offset;
        });

    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (it->fd != fd) { ++it; continue; }

        if (do_flush(*it) < 0) rc = -1;

        /* Release the slot this write used */
        for (auto& slot : pool_) {
            if (slot.mem == it->data) { release(&slot); break; }
        }
        it = pending_.erase(it);
    }
    return rc;
}

int BufferManager::flush_all() {
    std::lock_guard<std::mutex> lock(write_mu_);
    int rc = 0;
    for (auto& req : pending_) {
        if (do_flush(req) < 0) rc = -1;
        for (auto& slot : pool_) {
            if (slot.mem == req.data) { release(&slot); break; }
        }
    }
    pending_.clear();
    return rc;
}

size_t BufferManager::slots_in_use() const {
    std::lock_guard<std::mutex> lock(pool_mu_);
    size_t n = 0;
    for (const auto& s : pool_) if (s.in_use) ++n;
    return n;
}

size_t BufferManager::pending_writes() const {
    std::lock_guard<std::mutex> lock(write_mu_);
    return pending_.size();
}

} /* namespace fluxdm */
