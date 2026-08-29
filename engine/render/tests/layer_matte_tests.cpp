#include <mediaserver/render/layer_matte.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int failures{};

#define MATTE_CHECK(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__   \
                      << ": " #condition << '\n';                            \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

void cpu_alpha_contract() {
    using namespace mediaserver;
    const rhi::Color source[]{rhi::Color{0.4F, 0.1F, 0.05F, 0.5F}};
    const rhi::Color matte[]{rhi::Color{0.0F, 0.0F, 0.0F, 0.25F}};
    const auto result = render::matte_reference(
        source, matte, 1U, 1U,
        compositor::MatteParameters{compositor::MatteMode::Alpha, false,
                                      true});
    MATTE_CHECK(result.has_value());
    if (result) {
        MATTE_CHECK(std::fabs(result.value()[0].a - 0.125F) < 1.0e-6F);
        MATTE_CHECK(std::fabs(result.value()[0].r - 0.1F) < 1.0e-6F);
    }
}

} // namespace

int main() {
    cpu_alpha_contract();
    if (failures != 0) {
        std::cerr << failures << " Matte check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Layer Matte tests passed\n";
    return EXIT_SUCCESS;
}
