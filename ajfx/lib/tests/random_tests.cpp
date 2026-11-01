#include <ajfx/random.hpp>
#include <ajfx/version.hpp>

#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define AJFX_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The expected values were computed outside C++ (arbitrary-precision integers
// masked to 32 bits), so they pin the arithmetic rather than restate it.
void hash_mix_matches_reference_values() {
    AJFX_CHECK(ajfx::hash_mix(0U, 0U) == 0x01fce552U);
    AJFX_CHECK(ajfx::hash_mix(1U, 2U) == 0xa7bd535eU);
    AJFX_CHECK(ajfx::hash_mix(0xdeadbeefU, 0x12345678U) == 0xd4de87b2U);
    AJFX_CHECK(ajfx::hash_mix(0xffffffffU, 0xffffffffU) == 0x927cba85U);
    static_assert(ajfx::hash_mix(1U, 2U) == 0xa7bd535eU, "usable at compile time");
}

void particle_hash_matches_reference_values() {
    AJFX_CHECK(ajfx::particle_hash(42U, 7U, 3U) == 0x44d60924U);
    AJFX_CHECK(ajfx::particle_unit(42U, 7U, 3U) == 0.8360769748687744F);
}

void every_input_changes_the_result() {
    const std::uint32_t base = ajfx::particle_hash(42U, 7U, 3U);
    AJFX_CHECK(ajfx::particle_hash(43U, 7U, 3U) != base);
    AJFX_CHECK(ajfx::particle_hash(42U, 8U, 3U) != base);
    AJFX_CHECK(ajfx::particle_hash(42U, 7U, 4U) != base);
    // Swapped inputs must not collide, or a particle's speed and lifetime
    // streams could alias another particle's.
    AJFX_CHECK(ajfx::particle_hash(7U, 42U, 3U) != base);
}

void units_stay_in_range_and_spread() {
    std::set<std::uint32_t> buckets;
    for (std::uint32_t index = 0U; index < 4096U; ++index) {
        const float unit = ajfx::particle_unit(1U, index, 0U);
        AJFX_CHECK(unit >= 0.0F && unit < 1.0F);
        buckets.insert(static_cast<std::uint32_t>(unit * 64.0F));
    }
    AJFX_CHECK(buckets.size() == 64U);
    AJFX_CHECK(ajfx::hash_unit(0xffffffffU) < 1.0F);
    AJFX_CHECK(ajfx::hash_unit(0U) == 0.0F);
}

// The statements of a mix function, whitespace removed and lowercased so the
// C++ `0x...U` and HLSL `0x...u` literals compare equal.
std::vector<std::string> mix_statements(const std::string& path, const std::string& signature) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error{"cannot open " + path};
    }
    std::vector<std::string> statements;
    bool inside = false;
    for (std::string line; std::getline(file, line);) {
        if (!inside) {
            inside = line.find(signature) != std::string::npos;
            continue;
        }
        std::string normalized;
        for (const char c : line) {
            if (std::isspace(static_cast<unsigned char>(c)) == 0) {
                normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (normalized == "returnstate;") {
            return statements;
        }
        statements.push_back(normalized);
    }
    throw std::runtime_error{"no complete " + signature + " in " + path};
}

void hlsl_twin_has_identical_arithmetic() {
    const std::string root = AJFX_SHADER_INCLUDE_DIR;
    const auto hlsl = mix_statements(root + "/ajfx_random.hlsli", "uint ajfx_hash_mix(");
    const auto cpp = mix_statements(root + "/../include/ajfx/random.hpp", "hash_mix(std::uint32_t state");
    AJFX_CHECK(hlsl.size() == 6U);
    AJFX_CHECK(hlsl == cpp);
}

void linked_library_matches_headers() {
    AJFX_CHECK(ajfx::library_version() == ajfx::header_version);
}

} // namespace

int main() {
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"hash_mix_matches_reference_values", hash_mix_matches_reference_values},
        {"particle_hash_matches_reference_values", particle_hash_matches_reference_values},
        {"every_input_changes_the_result", every_input_changes_the_result},
        {"units_stay_in_range_and_spread", units_stay_in_range_and_spread},
        {"hlsl_twin_has_identical_arithmetic", hlsl_twin_has_identical_arithmetic},
        {"linked_library_matches_headers", linked_library_matches_headers},
    };
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
