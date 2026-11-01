#pragma once

#include "compile.hpp"
#include "document.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The coach: plain-language notes on a page, each with the fix one click
// away. It knows the mistakes that make an effect look cheap - particles that
// pop at the end of their life, smoke slicing into the floor, glow too dim to
// bloom, sparks falling through the ground - and says what to do rather than
// what is wrong.
namespace ajfx::studio {

enum class AdviceLevel : std::uint8_t {
    Tip,     // would look better
    Warning, // probably not what was meant
    Error,   // cannot play
};

struct Advice final {
    Uid layer{};
    // The block the note is about, or 0 for the layer itself.
    Uid block{};
    AdviceLevel level{AdviceLevel::Tip};
    std::string message;
    // Empty when there is nothing to press.
    std::string fix_label;
    std::function<void(Document&)> fix;
};

[[nodiscard]] std::vector<Advice> advise(const Document& document, const Compiled& compiled);

} // namespace ajfx::studio
