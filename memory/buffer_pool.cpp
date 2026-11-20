#include "memory/buffer_pool.hpp"

#include <cstdlib>
#include <limits>

namespace lx::memory {

Buffer::~Buffer() {
    if (data_) std::free(data_);
}

Buffer::Buffer(Buffer&& other) noexcept
    : data_(other.data_), size_(other.size_), size_class_(other.size_class_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.size_class_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (data_) std::free(data_);
        data_ = other.data_;
        size_ = other.size_;
        size_class_ = other.size_class_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.size_class_ = 0;
    }
    return *this;
}

std::size_t size_class_for(u64 bytes) {
    // Round up to the next power of two, starting at one page. The shift must
    // stop at the largest representable size class: shifting past it would wrap
    // to zero and loop forever, so a request larger than that is clamped to the
    // maximum (the allocation then simply fails rather than hanging).
    std::size_t size_class = 4096;
    const std::size_t max_class = std::numeric_limits<std::size_t>::max() / 2 + 1;
    while (size_class < bytes && size_class < max_class) size_class <<= 1;
    return size_class;
}

Result<Buffer> BufferPool::acquire(u64 bytes) {
    if (bytes == 0) {
        return Status::error(ErrorCode::InvalidArgument, "buffer size must be > 0");
    }
    const std::size_t size_class = size_class_for(bytes);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = free_blocks_.find(size_class);
    if (it != free_blocks_.end() && !it->second.empty()) {
        u8* block = it->second.back();
        it->second.pop_back();
        pooled_bytes_ -= size_class;
        ++hits_;
        return Buffer(block, size_class, size_class);
    }
    ++misses_;

    // Allocate outside the lock is preferable, but the size class is known and
    // contention here is low; keep it simple and correct.
#if defined(_WIN32)
    u8* block = static_cast<u8*>(_aligned_malloc(size_class, 64));
#else
    void* raw = nullptr;
    if (::posix_memalign(&raw, 64, size_class) != 0) raw = nullptr;
    u8* block = static_cast<u8*>(raw);
#endif
    if (block == nullptr) {
        return Status::error(ErrorCode::OutOfMemory,
                             "failed to allocate buffer of " +
                                 std::to_string(size_class) + " bytes");
    }
    return Buffer(block, size_class, size_class);
}

void BufferPool::release(Buffer&& buffer) {
    if (!buffer.valid()) return;
    const std::size_t size_class = buffer.size_class();
    // Take ownership of the block so the buffer's destructor leaves it alone.
    u8* block = buffer.release_ownership();

    std::lock_guard<std::mutex> lock(mutex_);
    free_blocks_[size_class].push_back(block);
    pooled_bytes_ += size_class;
}

void BufferPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [size_class, blocks] : free_blocks_) {
        for (u8* block : blocks) {
#if defined(_WIN32)
            _aligned_free(block);
#else
            std::free(block);
#endif
        }
        blocks.clear();
        (void)size_class;
    }
    free_blocks_.clear();
    pooled_bytes_ = 0;
}

BufferPool::Stats BufferPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.hits = hits_;
    s.misses = misses_;
    s.pooled_bytes = pooled_bytes_;
    for (const auto& [size_class, blocks] : free_blocks_) {
        s.pooled_blocks += blocks.size();
        (void)size_class;
    }
    return s;
}

}  // namespace lx::memory
