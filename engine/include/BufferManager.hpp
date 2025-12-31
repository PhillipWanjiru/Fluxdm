/* FluxDM — engine/include/BufferManager.hpp
 * Phase 2: Pre-allocated aligned buffer pool + write coalescing.
 * Eliminates malloc/free on the hot I/O path and reduces pwrite() syscalls
 * by merging small adjacent writes into single larger operations.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <atomic>

namespace fluxdm {

/* ── Write request ───────────────────────────────────────────────────────── */
struct WriteRequest {
    uint8_t *data;
    size_t   len;
    uint64_t file_offset;
    int      fd;
};

/* ── Buffer slot ─────────────────────────────────────────────────────────── */
struct BufferSlot {
    alignas(4096) uint8_t *mem  { nullptr };  /* 4KB-aligned memory     */
    size_t                 cap  { 0 };
    size_t                 len  { 0 };        /* bytes used             */
    uint64_t               file_offset { 0 }; /* where this data goes   */
    int                    fd   { -1 };
    bool                   in_use { false };
};

/* ── BufferManager ───────────────────────────────────────────────────────── */
class BufferManager {
public:
    static constexpr size_t DEFAULT_SLOT_SIZE  = 256 * 1024;   /* 256 KB per slot */
    static constexpr size_t DEFAULT_POOL_SIZE  = 32;           /* 32 slots = 8 MB */
    static constexpr size_t COALESCE_THRESHOLD = 64 * 1024;    /* merge writes < 64 KB gap */

    explicit BufferManager(size_t pool_size  = DEFAULT_POOL_SIZE,
                           size_t slot_size  = DEFAULT_SLOT_SIZE);
    ~BufferManager();

    /* No copy */
    BufferManager(const BufferManager&)            = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    /* ── Acquire / release ─────────────────────────────────────────────────── */

    /* Get a free buffer slot. Blocks if pool is exhausted (rare). */
    BufferSlot *acquire();

    /* Return slot to pool after write completes. */
    void release(BufferSlot *slot);

    /* ── Write coalescing ──────────────────────────────────────────────────── */

    /* Queue a write. Returns true if immediately flushed (slot full). */
    bool enqueue(int fd, uint64_t file_offset, const void *data, size_t len);

    /* Flush all pending writes for a file descriptor. */
    int flush(int fd);

    /* Flush all pending writes across all fds. */
    int flush_all();

    /* ── Stats ─────────────────────────────────────────────────────────────── */
    size_t pool_size()      const { return pool_.size(); }
    size_t slots_in_use()   const;
    size_t pending_writes() const;
    size_t total_flushed()  const { return total_flushed_; }
    size_t syscall_saved()  const { return syscalls_saved_; }

    /* Optional write-complete callback */
    using OnFlush = std::function<void(int fd, uint64_t offset, size_t bytes)>;
    void set_on_flush(OnFlush cb) { on_flush_ = std::move(cb); }

private:
    std::vector<BufferSlot>         pool_;
    std::vector<WriteRequest>       pending_;
    mutable std::mutex              pool_mu_;
    mutable std::mutex              write_mu_;
    size_t                          slot_size_;
    std::atomic<size_t>             total_flushed_  { 0 };
    std::atomic<size_t>             syscalls_saved_ { 0 };
    OnFlush                         on_flush_;

    int  do_flush(WriteRequest& req);
    bool coalesce(WriteRequest& a, const WriteRequest& b) const;
};

} /* namespace fluxdm */
