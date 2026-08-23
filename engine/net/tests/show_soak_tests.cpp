#include "show_playback_rig.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

// The V1 sustained-rate proof.
//
// The exit condition it exists to answer is: two nodes hold 120 fps for ten
// minutes with zero skipped show positions, the same show frame on both, and
// flat Director memory. A ten-minute run does not belong in the default suite,
// so this is a separate binary with the duration on the command line. The
// short default keeps it honest in CI; `--seconds=600` is the gate run.
namespace {

[[nodiscard]] bool parse_u64(std::string_view value, core::u64& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_rate(
    std::string_view value,
    show::FrameRate& rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (
        !parse_u64(value.substr(0U, separator), numerator)
        || !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }
    if (numerator == 0U || denominator == 0U || numerator > 1'000'000U
        || denominator > 1'000'000U) {
        return false;
    }
    rate = show::FrameRate{
        static_cast<core::u32>(numerator), static_cast<core::u32>(denominator)};
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SustainedOptions options{};
    options.seconds = 600U;
    options.report_seconds = 30U;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        constexpr std::string_view seconds_prefix = "--seconds=";
        constexpr std::string_view rate_prefix = "--rate=";
        constexpr std::string_view report_prefix = "--report-seconds=";
        if (argument.starts_with(seconds_prefix)) {
            if (!parse_u64(argument.substr(seconds_prefix.size()), options.seconds)
                || options.seconds == 0U || options.seconds > 86'400U) {
                std::cerr << "--seconds must be 1..86400\n";
                return 2;
            }
            continue;
        }
        if (argument.starts_with(rate_prefix)) {
            if (!parse_rate(argument.substr(rate_prefix.size()), options.rate)) {
                std::cerr << "--rate must be NUM[/DEN]\n";
                return 2;
            }
            continue;
        }
        if (argument.starts_with(report_prefix)) {
            if (!parse_u64(
                    argument.substr(report_prefix.size()), options.report_seconds)
                || options.report_seconds == 0U) {
                std::cerr << "--report-seconds must be at least 1\n";
                return 2;
            }
            continue;
        }
        std::cerr << "usage: mediaserver_show_soak [--seconds=N] "
                     "[--rate=NUM[/DEN]] [--report-seconds=N]\n";
        return 2;
    }

    std::cout << "soak starting seconds=" << options.seconds
              << " rate=" << options.rate.numerator << '/'
              << options.rate.denominator << '\n'
              << std::flush;
    try {
        run_sustained(options);
    } catch (const std::exception& error) {
        std::cerr << "[fail] sustained rate: " << error.what() << '\n';
        return 1;
    }
    std::cout << "[pass] sustained rate\n";
    return 0;
}
