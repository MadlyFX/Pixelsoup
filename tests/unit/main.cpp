#include "test_support.hpp"

#include <exception>
#include <iostream>

int main() {
    std::size_t failures = 0U;
    for (const auto& test : mediaserver::test::registry()) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }

    std::cout << (mediaserver::test::registry().size() - failures) << '/'
              << mediaserver::test::registry().size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}

