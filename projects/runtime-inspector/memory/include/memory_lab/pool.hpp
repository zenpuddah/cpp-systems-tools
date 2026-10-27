#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace memory_lab {

struct SlotState {
    std::size_t next;
    bool allocated;
};

// Single-threaded, non-owning fixed-size raw slots. Storage and typed metadata
// are separate, non-overlapping caller allocations, exclusively loaned while
// active. They must outlive the pool and its objects. Destroy an object before
// releasing its slot. Destruction of the pool itself runs no object destructors.
class FixedPool {
public:
    static constexpr std::size_t max_alignment = 256;

    FixedPool(std::span<std::byte> storage, std::span<SlotState> metadata,
              std::size_t payload, std::size_t alignment) noexcept
        : storage_(storage), metadata_(metadata), payload_(payload), alignment_(alignment),
          tail_(storage.size()) {
        constexpr auto limit = std::numeric_limits<std::size_t>::max();
        if (payload == 0 || alignment == 0 || alignment > max_alignment ||
            (alignment & (alignment - 1)) != 0 || payload > limit - (alignment - 1) ||
            metadata.size() > limit / sizeof(SlotState)) {
            return; // No caller metadata writes on invalid configurations.
        }
        reserved_ = metadata.size() * sizeof(SlotState);
        stride_ = ((payload + (alignment - 1)) / alignment) * alignment;
        if (!storage.empty()) {
            const auto remainder = reinterpret_cast<std::uintptr_t>(storage.data()) % alignment;
            const auto required = remainder == 0 ? 0 : alignment - remainder;
            // If alignment lies beyond this view, every byte is front slack;
            // never form an out-of-view pointer merely to align an empty pool.
            front_ = std::min(required, storage.size());
        }
        const auto available = storage.size() - front_;
        capacity_ = std::min(metadata.size(), available / stride_);
        tail_ = available - capacity_ * stride_;
        for (std::size_t i = 0; i < capacity_; ++i) {
            metadata_[i] = {i + 1 < capacity_ ? i + 1 : none, false};
        }
        head_ = capacity_ == 0 ? none : 0;
        valid_ = true;
    }

    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;
    FixedPool(FixedPool&&) = delete;
    FixedPool& operator=(FixedPool&&) = delete;

    void* allocate() noexcept {
        if (!valid_ || head_ == none) return nullptr;
        const auto index = head_;
        head_ = metadata_[index].next;
        metadata_[index].allocated = true;
        ++live_;
        return slot_start(index);
    }

    bool deallocate(void* pointer) noexcept {
        if (!valid_ || pointer == nullptr) return false;
        // Validation is O(capacity), separately from O(1) free-list removal/
        // insertion. Equality against generated starts avoids dereferencing an
        // untrusted pointer or subtracting/ordering unrelated pointers.
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (pointer != slot_start(i)) continue;
            if (!metadata_[i].allocated) return false;
            metadata_[i] = {head_, false};
            head_ = i;
            --live_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t payload_size() const noexcept { return payload_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
    [[nodiscard]] std::size_t slot_stride() const noexcept { return stride_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live_count() const noexcept { return live_; }
    [[nodiscard]] std::size_t free_count() const noexcept { return capacity_ - live_; }
    [[nodiscard]] std::size_t alignment_padding() const noexcept { return front_; }
    [[nodiscard]] std::size_t tail_bytes() const noexcept { return tail_; }
    [[nodiscard]] std::size_t metadata_reserved_bytes() const noexcept { return reserved_; }
    [[nodiscard]] std::size_t metadata_used_bytes() const noexcept { return capacity_ * sizeof(SlotState); }
    [[nodiscard]] std::size_t backing_capacity() const noexcept { return storage_.size(); }

private:
    void* slot_start(std::size_t index) const noexcept {
        // Only called for index < capacity_; all products/sums are bounded by
        // the constructor's division, and payload is nonzero.
        return storage_.data() + front_ + index * stride_;
    }

    static constexpr auto none = std::numeric_limits<std::size_t>::max();
    std::span<std::byte> storage_;
    std::span<SlotState> metadata_;
    std::size_t payload_, alignment_;
    std::size_t stride_ = 0, capacity_ = 0, live_ = 0;
    std::size_t front_ = 0, tail_, reserved_ = 0, head_ = none;
    bool valid_ = false;
};

} // namespace memory_lab
