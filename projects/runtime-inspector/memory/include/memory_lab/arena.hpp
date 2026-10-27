#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace memory_lab {

// Single-threaded, non-owning raw storage. The caller must destroy every live
// object before reset/reuse and keep the buffer alive. reset runs no destructor
// and invalidates all old handles; individual allocations cannot be freed.
class BumpArena {
public:
    static constexpr std::size_t max_alignment = 256;

    explicit BumpArena(std::span<std::byte> storage) noexcept : storage_(storage) {}
    BumpArena(const BumpArena&) = delete;
    BumpArena& operator=(const BumpArena&) = delete;
    BumpArena(BumpArena&&) = delete;
    BumpArena& operator=(BumpArena&&) = delete;

    void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
        if (bytes == 0 || alignment == 0 || alignment > max_alignment ||
            (alignment & (alignment - 1)) != 0 || bytes > remaining()) {
            return nullptr;
        }
        // bytes > 0 and bytes <= remaining(), so even an empty/null span never
        // reaches pointer arithmetic. std::align consumes padding, not payload.
        auto space = remaining();
        void* candidate = storage_.data() + consumed_;
        if (std::align(alignment, bytes, candidate, space) == nullptr) return nullptr;
        const auto padding = remaining() - space;
        // Successful std::align proves padding + bytes <= remaining().
        consumed_ += padding + bytes;
        payload_ += bytes;
        padding_ += padding;
        return candidate;
    }

    void reset() noexcept { consumed_ = payload_ = padding_ = 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t consumed() const noexcept { return consumed_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return capacity() - consumed_; }
    [[nodiscard]] std::size_t payload_bytes() const noexcept { return payload_; }
    [[nodiscard]] std::size_t padding_bytes() const noexcept { return padding_; }

private:
    std::span<std::byte> storage_;
    std::size_t consumed_ = 0;
    std::size_t payload_ = 0;
    std::size_t padding_ = 0;
};

} // namespace memory_lab
