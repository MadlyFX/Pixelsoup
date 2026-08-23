#include "test_support.hpp"

#include <mediaserver/doc/document.hpp>

#include <cstdint>
#include <string>

namespace {

using mediaserver::doc::ChangeKind;
using mediaserver::doc::Node;
using mediaserver::doc::NodeId;
using mediaserver::doc::Root;

[[nodiscard]] Node node(std::uint64_t id, std::string name, std::int64_t value) {
    return Node{NodeId{id}, std::move(name), {{"value", value}}};
}

MS_TEST("document updates preserve old snapshots and share untouched node values") {
    const Root empty;
    const auto one = empty.set(node(1U, "surface", 10));
    const auto* first_pointer = one.get(NodeId{1U});
    const auto two = one.set(node(2U, "layer", 20));

    MS_CHECK(empty.empty());
    MS_CHECK(empty.version().value() == 0U);
    MS_CHECK(one.size() == 1U);
    MS_CHECK(one.version().value() == 1U);
    MS_CHECK(two.size() == 2U);
    MS_CHECK(two.version().value() == 2U);
    MS_CHECK(one.get(NodeId{2U}) == nullptr);
    MS_CHECK(two.get(NodeId{1U}) == first_pointer);
}

MS_TEST("document diff reports deterministic add, modify, and remove changes") {
    const Root empty;
    const auto baseline = empty.set(node(1U, "surface", 10)).set(node(2U, "layer", 20));
    const auto modified = baseline.set(node(1U, "surface", 11));
    const auto erased = modified.erase(NodeId{2U});
    MS_CHECK(erased.has_value());
    const auto final = erased->set(node(3U, "effect", 30));

    const auto delta = mediaserver::doc::diff(baseline, final);
    MS_CHECK(delta.from == baseline.version());
    MS_CHECK(delta.to == final.version());
    MS_CHECK(delta.changes.size() == 3U);
    MS_CHECK(delta.changes[0].id == NodeId{1U});
    MS_CHECK(delta.changes[0].kind == ChangeKind::Modified);
    MS_CHECK(delta.changes[1].id == NodeId{2U});
    MS_CHECK(delta.changes[1].kind == ChangeKind::Removed);
    MS_CHECK(delta.changes[2].id == NodeId{3U});
    MS_CHECK(delta.changes[2].kind == ChangeKind::Added);
}

MS_TEST("erasing a missing node does not manufacture a new version") {
    const Root root;
    const auto erased = root.erase(NodeId{99U});
    MS_CHECK(!erased.has_value());
    MS_CHECK(root.version().value() == 0U);
}

} // namespace

