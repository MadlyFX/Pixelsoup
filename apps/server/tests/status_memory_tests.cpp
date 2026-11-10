#include "playback_runtime.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace show = mediaserver::show;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

constexpr std::uint64_t mib = 1024U * 1024U;

void a_measured_device_reports_every_field_in_mib() {
    const show::PresenterMemory memory{true, 9000U * mib, 22000U * mib,
                                       17500U * mib, 6100U * mib};
    CHECK(app::memory_text(memory, 19856U * mib) ==
          " gpu-local-mib=9000/22000 gpu-system-mib=17500"
          " gpu-committed-mib=6100 private-mib=19856");
}

void an_unknown_device_prints_no_gpu_fields() {
    CHECK(app::memory_text(show::PresenterMemory{}, 512U * mib) ==
          " private-mib=512");
}

void nothing_measured_prints_nothing() {
    CHECK(app::memory_text(show::PresenterMemory{}, 0U).empty());
}

void partial_mebibytes_round_down() {
    const show::PresenterMemory memory{true, mib - 1U, 2U * mib + 7U, 0U, 0U};
    CHECK(app::memory_text(memory, 0U) ==
          " gpu-local-mib=0/2 gpu-system-mib=0 gpu-committed-mib=0");
}

void this_process_reports_private_bytes_where_the_platform_can() {
#if defined(_WIN32)
    CHECK(app::process_private_bytes() > 0U);
#else
    CHECK(app::process_private_bytes() == 0U);
#endif
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array cases{
    Case{"a measured device reports every field in MiB",
         &a_measured_device_reports_every_field_in_mib},
    Case{"an unknown device prints no GPU fields",
         &an_unknown_device_prints_no_gpu_fields},
    Case{"nothing measured prints nothing", &nothing_measured_prints_nothing},
    Case{"partial mebibytes round down", &partial_mebibytes_round_down},
    Case{"this process reports private bytes where the platform can",
         &this_process_reports_private_bytes_where_the_platform_can},
};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
