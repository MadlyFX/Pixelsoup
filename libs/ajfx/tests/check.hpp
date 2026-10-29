#pragma once

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The tiny harness the ajfx tests share: a failed check throws, and the
// runner reports each case and returns non-zero if any failed.
namespace ajfx::test {

inline void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

struct Case final {
    const char* name;
    void (*run)();
};

template <std::size_t N>
int run(const Case (&cases)[N]) {
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace ajfx::test

#define AJFX_CHECK(...) ::ajfx::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
