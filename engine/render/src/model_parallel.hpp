#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace mediaserver::render {
// Worker-only image work. Each row has a fixed destination and independent
// arithmetic, so scheduling cannot change the generated package bytes.
template<class Function>
void model_parallel_rows(std::size_t rows, Function function) {
    const auto workers = std::min<std::size_t>(8U,
        std::max(1U, std::thread::hardware_concurrency() / 2U));
    if (rows < 64U || workers == 1U) {
        for (std::size_t row = 0; row < rows; ++row) function(row);
        return;
    }
    std::atomic<std::size_t> next{};
    const auto work = [&] {
        for (;;) {
            const auto first = next.fetch_add(8U, std::memory_order_relaxed);
            if (first >= rows) return;
            for (auto row = first; row < std::min(first + 8U, rows); ++row) function(row);
        }
    };
    std::vector<std::jthread> threads;
    threads.reserve(workers - 1U);
    for (std::size_t i = 1; i < workers; ++i) threads.emplace_back(work);
    work();
}
} // namespace mediaserver::render
