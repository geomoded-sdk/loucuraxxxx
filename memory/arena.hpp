// LoucuraXXXX GPU - bump/arena allocator for scratch and staging memory.
//
// Arenas back the CPU-side working buffers used by filters and the software
// render path. A single large allocation is split into aligned sub-allocations
// and released all at once, which keeps the hot paths free of per-frame
// malloc/free churn.
#pragma once

#include <cstddef>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::memory {

class Arena {
public:
    Arena() = default;
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    // Allocates the backing block. Fails if the requested size is zero.
    Status reserve(u64 bytes);

    // Releases the backing block. All previously returned pointers become
    // invalid.
    void reset();

    // Resets the bump pointer without freeing the backing block. Pointers from
    // the previous generation become invalid.
    void rewind() { offset_ = 0; }

    // Returns an aligned pointer, or nullptr when the arena is exhausted.
    // alignment must be a power of two.
    void* allocate(u64 bytes, u64 alignment = 64);

    template <typename T>
    T* allocate_array(u64 count) {
        // sizeof(T) * count can wrap for a large count, which would hand back a
        // block far smaller than the caller expects.
        if (count > ~static_cast<u64>(0) / sizeof(T)) return nullptr;
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    u64 capacity() const { return capacity_; }
    u64 used() const { return offset_; }
    u64 high_water_mark() const { return high_water_mark_; }
    bool empty() const { return offset_ == 0; }

private:
    u8* base_ = nullptr;
    u64 capacity_ = 0;
    u64 offset_ = 0;
    u64 high_water_mark_ = 0;
};

}  // namespace lx::memory
