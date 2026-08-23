#pragma once

#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mediaserver::show::test {

struct Case final {
    std::string_view name;
    void (*function)();
};

inline void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

[[nodiscard]] inline int run(std::span<const Case> cases) {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}

} // namespace mediaserver::show::test

#define SHOW_CHECK(expression)                                                 \
    ::mediaserver::show::test::check(                                          \
        static_cast<bool>(expression), #expression, __LINE__)
