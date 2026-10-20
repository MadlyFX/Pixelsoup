#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/show/presenter.hpp>

#include "show_test_support.hpp"

#include <array>

namespace {

namespace show = mediaserver::show;
namespace core = mediaserver::core;

struct Extent final {
    core::u32 width;
    core::u32 height;
};

// Output windows span Mosaic desktops, so a surface wider or taller than one
// 8K display must open as long as it stays within the authored output limit.
void surfaces_open_up_to_the_authored_output_limit() {
    constexpr std::array<Extent, 4U> accepted{{
        {7'681U, 2'160U},
        {15'360U, 2'160U},
        {3'840U, 8'640U},
        {16'384U, 1'080U},
    }};
    for (const Extent extent : accepted) {
        auto created = show::PresenterSurface::create(extent.width, extent.height);
        SHOW_CHECK(created.has_value());
        const auto& surface = created.value();
        SHOW_CHECK(surface.width() == extent.width);
        SHOW_CHECK(surface.height() == extent.height);
        SHOW_CHECK(surface.stride() == extent.width * 4U);
    }
}

void surfaces_beyond_the_output_limit_are_refused() {
    constexpr std::array<Extent, 4U> rejected{{
        {0U, 1'080U},
        {1'920U, 0U},
        {core::max_output_dimension + 1U, 1'080U},
        {1'920U, core::max_output_dimension + 1U},
    }};
    for (const Extent extent : rejected) {
        const auto created =
            show::PresenterSurface::create(extent.width, extent.height);
        SHOW_CHECK(!created.has_value());
        SHOW_CHECK(created.error() == show::PresenterError::InvalidConfiguration);
    }
}

constexpr std::array<show::test::Case, 2U> cases{{
    {"surfaces open up to the authored output limit",
     &surfaces_open_up_to_the_authored_output_limit},
    {"surfaces beyond the output limit are refused",
     &surfaces_beyond_the_output_limit_are_refused},
}};

} // namespace

int main() {
    return show::test::run(cases);
}
