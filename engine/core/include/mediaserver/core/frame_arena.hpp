#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace mediaserver::core {

class FrameArena final {
public:
    explicit FrameArena(std::span<std::byte> storage) noexcept : storage_(storage) {}

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t byte_count,
        std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
        const auto current = base + offset_;
        const auto aligned = (current + alignment - 1U) & ~(alignment - 1U);
        const auto padding = static_cast<std::size_t>(aligned - current);

        if (padding > remaining() || byte_count > remaining() - padding) {
            return nullptr;
        }

        offset_ += padding + byte_count;
        return reinterpret_cast<void*>(aligned);
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* make(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        static_assert(
            std::is_trivially_destructible_v<T>,
            "FrameArena does not run destructors when reset");
        void* memory = allocate(sizeof(T), alignof(T));
        if (memory == nullptr) {
            return nullptr;
        }
        return ::new (memory) T(std::forward<Args>(args)...);
    }

    void reset() noexcept { offset_ = 0U; }

    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return capacity() - used(); }

private:
    std::span<std::byte> storage_;
    std::size_t offset_{};
};

} // namespace mediaserver::core

