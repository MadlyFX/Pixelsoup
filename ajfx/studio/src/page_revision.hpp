#pragma once

#include "document.hpp"

#include <cstdint>

// Knowing the page has changed without asking again whether it has.
//
// The page is edited in place by whatever draws it, so nothing announces a
// change; the one way to find one is to compare the page with itself as it
// was. Done by everything that cares -- the preview's compile, the saved
// marker, the coach, the history -- that is a deep comparison apiece, every
// frame. PageRevision makes it once and hands out a number, and PageMark
// answers "does the page differ from this copy" from that number, working it
// out again only when the page or the copy has moved.
namespace ajfx::studio {

class PageRevision final {
public:
    // Looks at the page: one comparison with the page as last seen. The
    // number moves when anything in it differs. Call once a frame, after
    // whatever edits the page in place has run.
    std::uint64_t observe(const Document& document) {
        if (!(document == seen_)) {
            seen_ = document;
            ++revision_;
        }
        return revision_;
    }

    // The page was replaced, or stepped back to an earlier one, as a whole:
    // it has changed, and no comparison is needed to know.
    std::uint64_t touch(const Document& document) {
        seen_ = document;
        return ++revision_;
    }

    // The number as of the last look; it moves only by observe and touch.
    [[nodiscard]] std::uint64_t value() const noexcept { return revision_; }

private:
    Document seen_;
    std::uint64_t revision_{1U};
};

// A copy of the page kept to tell later whether the page has moved from it:
// as saved, as sent to the preview, as compiled, as the history's newest step.
class PageMark final {
public:
    // Keeps `document`, which stands at `revision`.
    void set(const Document& document, std::uint64_t revision) {
        copy_ = document;
        ++generation_;
        asked_revision_ = revision;
        asked_generation_ = generation_;
        differs_ = false;
    }

    [[nodiscard]] const Document& copy() const noexcept { return copy_; }

    // Whether `document`, at `revision`, differs from the copy. The pages are
    // compared when the page has changed or the copy was replaced since last
    // asked, and the answer is kept otherwise. A page changed and changed back
    // is the page it was, so it does not differ.
    [[nodiscard]] bool differs(const Document& document, std::uint64_t revision) {
        if (asked_revision_ != revision || asked_generation_ != generation_) {
            asked_revision_ = revision;
            asked_generation_ = generation_;
            differs_ = !(document == copy_);
        }
        return differs_;
    }

private:
    Document copy_;
    std::uint64_t generation_{1U};
    std::uint64_t asked_revision_{0U};
    std::uint64_t asked_generation_{0U};
    bool differs_{true};
};

} // namespace ajfx::studio
