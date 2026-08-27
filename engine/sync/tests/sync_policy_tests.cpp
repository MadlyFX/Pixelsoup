#include <mediaserver/sync/sync_policy_model.hpp>

#include <mediaserver/sync/sync_schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace schema = mediaserver::sync::schema;
namespace sync = mediaserver::sync;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define SYNC_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{type};
    for (const auto& property : properties) {
        built.properties.emplace(property.first, property.second);
    }
    return built;
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return Property{std::string{name}, doc::PropertyValue{std::string{value}}};
}

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] doc::Root with(const doc::Root& root, doc::Node value) {
    return root.set(std::move(value));
}

void an_unauthored_project_is_unsynced() {
    sync::SyncPolicyModel model;
    model.reset(doc::Root{});
    const sync::SyncPolicy policy = model.evaluate();
    SYNC_CHECK(!policy.authored);
    SYNC_CHECK(policy.mode == sync::SyncMode::Unsynced);
    SYNC_CHECK(!policy.requires_physical_sync());
    SYNC_CHECK(policy.failure == sync::SyncFailurePolicy::Strict);
    SYNC_CHECK(policy.late_frame == sync::LateFramePolicy::HoldGroup);
    SYNC_CHECK(model.counters().policies == 0U);
}

void an_authored_policy_round_trips_every_field() {
    sync::SyncPolicy authored{};
    authored.mode = sync::SyncMode::ExternalGenlock;
    authored.required_timing = sync::SyncTiming{3840U, 2160U, 60000U, 1001U};
    authored.group = 7U;
    authored.barrier = 3U;
    authored.failure = sync::SyncFailurePolicy::Degraded;
    authored.late_frame = sync::LateFramePolicy::AdvanceIndependently;

    doc::Node written{};
    written.id = doc::NodeId{41U};
    sync::write_sync_policy_properties(written, authored);

    sync::SyncPolicyModel model;
    model.reset(with(doc::Root{}, written));
    const sync::SyncPolicy policy = model.evaluate();
    SYNC_CHECK(policy.authored);
    SYNC_CHECK(policy.node == doc::NodeId{41U});
    SYNC_CHECK(policy.mode == sync::SyncMode::ExternalGenlock);
    SYNC_CHECK(policy.required_timing == authored.required_timing);
    SYNC_CHECK(policy.group == 7U);
    SYNC_CHECK(policy.barrier == 3U);
    SYNC_CHECK(policy.failure == sync::SyncFailurePolicy::Degraded);
    SYNC_CHECK(policy.late_frame ==
               sync::LateFramePolicy::AdvanceIndependently);
    SYNC_CHECK(policy.requires_physical_sync());
    SYNC_CHECK(policy.requires_external_reference());
}

void an_invalid_policy_node_is_rejected_and_stays_unsynced() {
    sync::SyncPolicyModel model;
    model.reset(with(doc::Root{},
                     node(9U, schema::policy_type,
                          {text(schema::mode, "sort-of-locked"),
                           whole(schema::width, 1920),
                           whole(schema::height, 1080)})));
    SYNC_CHECK(model.counters().rejected == 1U);
    SYNC_CHECK(model.evaluate().mode == sync::SyncMode::Unsynced);

    // A zero barrier id is not a barrier; refusing keeps a corrupt document
    // from silently widening what the project claims about physical timing.
    sync::SyncPolicyModel zero_barrier;
    zero_barrier.reset(with(doc::Root{},
                            node(10U, schema::policy_type,
                                 {text(schema::mode, schema::framelock_mode),
                                  whole(schema::barrier, 0)})));
    SYNC_CHECK(zero_barrier.counters().rejected == 1U);
    SYNC_CHECK(!zero_barrier.evaluate().requires_physical_sync());

    sync::SyncPolicyModel oversized;
    oversized.reset(with(doc::Root{},
                         node(11U, schema::policy_type,
                              {text(schema::mode, schema::framelock_mode),
                               whole(schema::width, 100'000)})));
    SYNC_CHECK(oversized.counters().rejected == 1U);
}

void the_lowest_policy_node_wins_deterministically() {
    doc::Root root;
    root = with(root, node(80U, schema::policy_type,
                           {text(schema::mode, schema::framelock_mode)}));
    root = with(root,
                node(20U, schema::policy_type,
                     {text(schema::mode, schema::external_genlock_mode)}));
    sync::SyncPolicyModel model;
    model.reset(root);
    SYNC_CHECK(model.counters().policies == 2U);
    SYNC_CHECK(model.evaluate().node == doc::NodeId{20U});
    SYNC_CHECK(model.evaluate().mode == sync::SyncMode::ExternalGenlock);
}

void removing_the_policy_node_returns_to_unsynced() {
    doc::Root root =
        with(doc::Root{}, node(5U, schema::policy_type,
                               {text(schema::mode, schema::framelock_mode)}));
    sync::SyncPolicyModel model;
    model.reset(root);
    SYNC_CHECK(model.evaluate().requires_physical_sync());

    const auto erased = root.erase(doc::NodeId{5U});
    SYNC_CHECK(erased.has_value());
    model.apply(diff(root, *erased));
    SYNC_CHECK(!model.evaluate().requires_physical_sync());
    SYNC_CHECK(model.counters().policies == 0U);
}

void refresh_comparison_is_exact_rational() {
    const sync::SyncTiming integral{1920U, 1080U, 60U, 1U};
    const sync::SyncTiming fractional{1920U, 1080U, 60000U, 1001U};
    const sync::SyncTiming scaled{1920U, 1080U, 120U, 2U};
    SYNC_CHECK(sync::same_refresh(integral, scaled));
    SYNC_CHECK(!sync::same_refresh(integral, fractional));
    SYNC_CHECK(sync::same_raster(integral, fractional));
    SYNC_CHECK(!sync::same_timing(integral, fractional));
    SYNC_CHECK(!sync::same_refresh(integral, sync::SyncTiming{1920U, 1080U,
                                                              60U, 0U}));
}

void every_authored_field_contributes_to_the_digest() {
    sync::SyncPolicy base{};
    base.authored = true;
    base.node = doc::NodeId{3U};
    base.mode = sync::SyncMode::Framelock;
    const core::u64 reference = sync::digest(base);

    sync::SyncPolicy changed = base;
    changed.barrier = 2U;
    SYNC_CHECK(sync::digest(changed) != reference);

    changed = base;
    changed.late_frame = sync::LateFramePolicy::AdvanceIndependently;
    SYNC_CHECK(sync::digest(changed) != reference);

    changed = base;
    changed.required_timing.refresh_denominator = 1001U;
    SYNC_CHECK(sync::digest(changed) != reference);

    changed = base;
    changed.failure = sync::SyncFailurePolicy::Degraded;
    SYNC_CHECK(sync::digest(changed) != reference);

    SYNC_CHECK(sync::digest(base) == reference);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"an unauthored project is unsynced", &an_unauthored_project_is_unsynced},
    {"an authored policy round trips every field",
     &an_authored_policy_round_trips_every_field},
    {"an invalid policy node is rejected and stays unsynced",
     &an_invalid_policy_node_is_rejected_and_stays_unsynced},
    {"the lowest policy node wins deterministically",
     &the_lowest_policy_node_wins_deterministically},
    {"removing the policy node returns to unsynced",
     &removing_the_policy_node_returns_to_unsynced},
    {"refresh comparison is exact rational",
     &refresh_comparison_is_exact_rational},
    {"every authored field contributes to the digest",
     &every_authored_field_contributes_to_the_digest},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
