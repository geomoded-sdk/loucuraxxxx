#include "memory/arena.hpp"

#include <cstdlib>
#include <utility>

namespace lx::memory {

namespace {
u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
}  // namespace

Arena::~Arena() { reset(); }

Arena::Arena(Arena&& other) noexcept
    : base_(other.base_),
      capacity_(other.capacity_),
      offset_(other.offset_),
      high_water_mark_(other.high_water_mark_) {
    other.base_ = nullptr;
    other.capacity_ = 0;
    other.offset_ = 0;
    other.high_water_mark_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        reset();
        base_ = other.base_;
        capacity_ = other.capacity_;
        offset_ = other.offset_;
        high_water_mark_ = other.high_water_mark_;
        other.base_ = nullptr;
        other.capacity_ = 0;
        other.offset_ = 0;
        other.high_water_mark_ = 0;
    }
    return *this;
}

Status Arena::reserve(u64 bytes) {
    if (bytes == 0) {
        return Status::error(ErrorCode::InvalidArgument, "arena size must be > 0");
    }
    reset();
#if defined(_WIN32)
    base_ = static_cast<u8*>(_aligned_malloc(bytes, 64));
#else
    if (::posix_memalign(reinterpret_cast<void**>(&base_), 64, bytes) != 0) {
        base_ = nullptr;
    }
#endif
    if (base_ == nullptr) {
        return Status::error(ErrorCode::OutOfMemory,
                             "failed to reserve " + std::to_string(bytes) + " bytes");
    }
    capacity_ = bytes;
    offset_ = 0;
    high_water_mark_ = 0;
    return Status::success();
}

void Arena::reset() {
#if defined(_WIN32)
    if (base_) _aligned_free(base_);
#else
    std::free(base_);
#endif
    base_ = nullptr;
    capacity_ = 0;
    offset_ = 0;
    high_water_mark_ = 0;
}

void* Arena::allocate(u64 bytes, u64 alignment) {
    if (base_ == nullptr || bytes == 0) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    // Reject requests that cannot fit before aligning, so neither align_up nor
    // start+bytes can wrap past the capacity check.
    if (bytes > capacity_) return nullptr;
    if (offset_ > capacity_ - bytes) return nullptr;
    const u64 start = align_up(offset_, alignment);
    if (start > capacity_ - bytes) return nullptr;
    void* ptr = base_ + start;
    offset_ = start + bytes;
    if (offset_ > high_water_mark_) high_water_mark_ = offset_;
    return ptr;
}

}  // namespace lx::memory
