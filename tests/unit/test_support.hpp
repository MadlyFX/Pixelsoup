#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::test {

struct TestCase final {
    std::string_view name;
    void (*function)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar final {
public:
    Registrar(std::string_view name, void (*function)()) {
        registry().push_back(TestCase{name, function});
    }
};

class Failure final : public std::runtime_error {
public:
    Failure(const char* expression, const char* file, int line)
        : std::runtime_error(
              std::string(file) + ':' + std::to_string(line) + ": check failed: " + expression) {}
};

inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw Failure(expression, file, line);
    }
}

} // namespace mediaserver::test

#define MS_TEST_JOIN_DETAIL(left, right) left##right
#define MS_TEST_JOIN(left, right) MS_TEST_JOIN_DETAIL(left, right)
#define MS_TEST_DETAIL(name, line)                                                      \
    static void MS_TEST_JOIN(ms_test_function_, line)();                                \
    static ::mediaserver::test::Registrar MS_TEST_JOIN(ms_test_registrar_, line)(       \
        name, &MS_TEST_JOIN(ms_test_function_, line));                                  \
    static void MS_TEST_JOIN(ms_test_function_, line)()
#define MS_TEST(name) MS_TEST_DETAIL(name, __LINE__)
#define MS_CHECK(expression)                                                            \
    ::mediaserver::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

