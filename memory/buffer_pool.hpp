// LoucuraXXXX GPU - thread-safe pool of reusable host buffers.
//
// Filters and the staging path need same-sized scratch buffers every frame.
// Reusing them avoids repeated page faults and allocator contention, which is
// the single largest source of avoidable overhead in the CPU filter path.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::memory {

// A block of host memory with an explicit size. Move-only.
class Buffer {
public:
    Buffer() = default;
    Buffer(u8* data, u64 size, std::size_t size_class)
        : data_(data), size_(size), size_class_(size_class) {}
    ~Buffer();

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    u8* data() { return data_; }
    const u8* data() const { return data_; }
    u64 size() const { return size_; }
    std::size_t size_class() const { return size_class_; }
    bool valid() const { return data_ != nullptr; }

    // Relinquishes ownership of the backing block and returns it. The caller
    // becomes responsible for freeing it. Used by BufferPool.
    u8* release_ownership() {
        u8* block = data_;
        data_ = nullptr;
        return block;
    }

private:
    u8* data_ = nullptr;
    u64 size_ = 0;
    std::size_t size_class_ = 0;
};

// Rounds a byte count up to the next power-of-two size class, with a floor.
std::size_t size_class_for(u64 bytes);

class BufferPool {
public:
    BufferPool() = default;
    // Pooled blocks are owned by the pool, so destroying it must free them.
    ~BufferPool() { clear(); }
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Retrieves a buffer of at least `bytes`, reusing a pooled block when one
    // is free. Returns OutOfMemory when a new block cannot be allocated.
    Result<Buffer> acquire(u64 bytes);

    // Returns a buffer to the pool. Empty buffers are ignored.
    void release(Buffer&& buffer);

    // Drops all cached blocks. Live buffers are unaffected.
    void clear();

    // Statistics for telemetry.
    struct Stats {
        std::size_t pooled_blocks = 0;
        u64 pooled_bytes = 0;
        u64 hits = 0;
        u64 misses = 0;
    };
    Stats stats() const;

private:
    mutable std::mutex mutex_;
    std::map<std::size_t, std::vector<u8*>> free_blocks_;
    u64 pooled_bytes_ = 0;
    u64 hits_ = 0;
    u64 misses_ = 0;
};

}  // namespace lx::memory
