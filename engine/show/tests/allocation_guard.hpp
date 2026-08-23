#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

// The frame-boundary allocation contract, enforced rather than asserted in
// prose. Every global allocation entry point is replaced so a scoped guard can
// prove that a boundary operation neither allocates nor frees. Deallocation
// counts too: free() takes the same allocator lock malloc() does, and a trie
// released on the boundary is exactly as damaging as one acquired there.

namespace mediaserver::show::test {

// Constant-initialised, so these are live before any dynamic initialiser can
// run an allocation through the replaced operators.
inline std::atomic<std::size_t> allocation_count{0U};
inline std::atomic<std::size_t> deallocation_count{0U};

// Counts only while armed, which keeps test scaffolding, iostreams, and
// container growth outside the measured region.
inline std::atomic<bool> counting{false};

class AllocationGuard final {
public:
    AllocationGuard() noexcept
        : allocations_(allocation_count.load(std::memory_order_relaxed)),
          deallocations_(deallocation_count.load(std::memory_order_relaxed)) {
        counting.store(true, std::memory_order_relaxed);
    }

    AllocationGuard(const AllocationGuard&) = delete;
    AllocationGuard& operator=(const AllocationGuard&) = delete;
    AllocationGuard(AllocationGuard&&) = delete;
    AllocationGuard& operator=(AllocationGuard&&) = delete;

    ~AllocationGuard() { counting.store(false, std::memory_order_relaxed); }

    void disarm() noexcept { counting.store(false, std::memory_order_relaxed); }

    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocation_count.load(std::memory_order_relaxed) - allocations_;
    }
    [[nodiscard]] std::size_t deallocations() const noexcept {
        return deallocation_count.load(std::memory_order_relaxed)
            - deallocations_;
    }
    [[nodiscard]] bool quiet() const noexcept {
        return allocations() == 0U && deallocations() == 0U;
    }

private:
    std::size_t allocations_{};
    std::size_t deallocations_{};
};

inline void record_allocation() noexcept {
    if (counting.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

inline void record_deallocation() noexcept {
    if (counting.load(std::memory_order_relaxed)) {
        deallocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline void* allocate(std::size_t size) {
    record_allocation();
    // Zero-sized requests still have to return distinct pointers.
    void* memory = std::malloc(size == 0U ? 1U : size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

[[nodiscard]] inline void* allocate_nothrow(std::size_t size) noexcept {
    record_allocation();
    return std::malloc(size == 0U ? 1U : size);
}

inline void release(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    record_deallocation();
    std::free(memory);
}

[[nodiscard]] inline void* allocate_aligned(
    std::size_t size,
    std::size_t alignment) noexcept {
    record_allocation();
    const auto request = size == 0U ? 1U : size;
#if defined(_MSC_VER)
    return _aligned_malloc(request, alignment);
#else
    const auto rounded = (request + alignment - 1U) / alignment * alignment;
    return std::aligned_alloc(alignment, rounded);
#endif
}

inline void release_aligned(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    record_deallocation();
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace mediaserver::show::test

void* operator new(std::size_t size) {
    return mediaserver::show::test::allocate(size);
}

void* operator new[](std::size_t size) {
    return mediaserver::show::test::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return mediaserver::show::test::allocate_nothrow(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return mediaserver::show::test::allocate_nothrow(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* memory = mediaserver::show::test::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    void* memory = mediaserver::show::test::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new(
    std::size_t size,
    std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    return mediaserver::show::test::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](
    std::size_t size,
    std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    return mediaserver::show::test::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete[](void* memory) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    mediaserver::show::test::release(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    mediaserver::show::test::release_aligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    mediaserver::show::test::release_aligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    mediaserver::show::test::release_aligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    mediaserver::show::test::release_aligned(memory);
}

void operator delete(
    void* memory,
    std::align_val_t,
    const std::nothrow_t&) noexcept {
    mediaserver::show::test::release_aligned(memory);
}

void operator delete[](
    void* memory,
    std::align_val_t,
    const std::nothrow_t&) noexcept {
    mediaserver::show::test::release_aligned(memory);
}
