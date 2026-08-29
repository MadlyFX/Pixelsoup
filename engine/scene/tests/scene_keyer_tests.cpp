#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define KEYER_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return {std::string{name}, std::string{value}};
}

[[nodiscard]] Property boolean(std::string_view name, bool value) {
    return {std::string{name}, value};
}

[[nodiscard]] doc::Node node(
    core::u64 id, std::string_view type,
    std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& [name, value] : properties) {
        result.properties.emplace(name, value);
    }
    return result;
}

[[nodiscard]] doc::Node chroma_node(core::u64 id = 20U,
                                    double mix = 0.8) {
    return node(
        id, schema::effect_type,
        {whole(schema::effect_target, 10),
         text(schema::effect_kind, schema::chroma_key_kind),
         whole(schema::effect_order, -3),
         real(schema::chroma_key_color_r, 0.125),
         real(schema::chroma_key_color_g, 0.75),
         real(schema::chroma_key_color_b, 0.25),
         real(schema::chroma_key_tolerance, 0.2),
         real(schema::chroma_key_softness, 0.05),
         whole(schema::chroma_key_morphology_px, -4),
         real(schema::chroma_key_spill_suppression, 0.6),
         real(schema::chroma_key_mix, mix)});
}

[[nodiscard]] doc::Node luma_node(core::u64 id = 21U,
                                  double mix = 0.9) {
    return node(
        id, schema::effect_type,
        {whole(schema::effect_target, 10),
         text(schema::effect_kind, schema::luma_key_kind),
         whole(schema::effect_order, 7),
         real(schema::luma_key_low_threshold, 0.2),
         real(schema::luma_key_high_threshold, 0.8),
         real(schema::luma_key_low_softness, 0.1),
         real(schema::luma_key_high_softness, 0.15),
         boolean(schema::luma_key_invert, true),
         real(schema::luma_key_mix, mix)});
}

[[nodiscard]] doc::Node channel(core::u64 id, core::u64 target,
                                std::string_view component, double value) {
    doc::Node result = node(
        id, schema::channel_type,
        {whole(schema::channel_target, static_cast<std::int64_t>(target)),
         text(schema::channel_component, component)});
    result.properties.emplace(
        std::string{schema::channel_key_prefix} + "0"
            + std::string{schema::channel_key_frame_suffix},
        std::int64_t{0});
    result.properties.emplace(
        std::string{schema::channel_key_prefix} + "0"
            + std::string{schema::channel_key_value_suffix},
        value);
    return result;
}

[[nodiscard]] doc::Root keyer_root(bool with_channels = true) {
    doc::Root result;
    result = result.set(node(1U, schema::stage_type));
    result = result.set(
        node(10U, schema::object_type, {whole(schema::scene_id, 1)}));
    result = result.set(chroma_node());
    result = result.set(luma_node());
    if (with_channels) {
        result = result.set(
            channel(30U, 20U, schema::chroma_key_mix, 0.25));
        result = result.set(
            channel(31U, 21U, schema::luma_key_mix, 0.5));
    }
    return result;
}

[[nodiscard]] scene::Scene evaluate(const scene::SceneModel& model) {
    return model.evaluate(doc::NodeId{1U}, scene::SceneTime{0U, 60U, 1U});
}

[[nodiscard]] bool same_document(const doc::Root& left,
                                 const doc::Root& right) {
    return left.version() == right.version()
        && doc::diff(left, right).changes.empty();
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-scene-keyer-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        const bool made = std::filesystem::create_directory(path_, error);
        if (!made || error) {
            throw std::runtime_error{"could not create keyer test directory"};
        }
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] std::filesystem::path file() const {
        return path_ / "keyers.msproject";
    }

private:
    std::filesystem::path path_;
};

void typed_projection_and_primary_channels_preserve_order() {
    const doc::Root root = keyer_root();
    scene::SceneModel model;
    model.reset(root);
    const auto authored = model.effects();
    KEYER_CHECK(authored.size() == 2U);
    KEYER_CHECK(authored[0U].kind == scene::EffectKind::ChromaKey);
    KEYER_CHECK(authored[0U].chroma_key ==
                (scene::ChromaKeyParameters{
                    {0.125F, 0.75F, 0.25F}, 0.2F, 0.05F, -4, 0.6F,
                    0.8F}));
    KEYER_CHECK(authored[1U].kind == scene::EffectKind::LumaKey);
    KEYER_CHECK(authored[1U].luma_key ==
                (scene::LumaKeyParameters{
                    0.2F, 0.8F, 0.1F, 0.15F, true, 0.9F}));

    const scene::Scene frame = evaluate(model);
    KEYER_CHECK(frame.effects.size() == 2U);
    KEYER_CHECK(frame.effects[0U].chroma_key.mix == 0.25F);
    KEYER_CHECK(frame.effects[1U].luma_key.mix == 0.5F);
    KEYER_CHECK(frame.instances.size() == 1U);
    const auto& operations =
        frame.instances.front().style.effects.spatial_operations;
    KEYER_CHECK(operations.size() == 2U);
    KEYER_CHECK(operations[0U].kind == scene::EffectKind::ChromaKey);
    KEYER_CHECK(operations[0U].chroma_key.mix == 0.25F);
    KEYER_CHECK(operations[1U].kind == scene::EffectKind::LumaKey);
    KEYER_CHECK(operations[1U].luma_key.mix == 0.5F);

    const std::array addresses{
        scene::KeyAddress{doc::NodeId{20U},
                          scene::PropertyChannel::ChromaKeyMix, 0},
        scene::KeyAddress{doc::NodeId{21U},
                          scene::PropertyChannel::LumaKeyMix, 0},
    };
    const auto selected = scene::inspect_key_selection(root, addresses);
    KEYER_CHECK(selected.has_value());
    KEYER_CHECK(selected.value().records.size() == 2U);
    KEYER_CHECK(selected.value().skips.empty());
    KEYER_CHECK(selected.value().records[0U].channel == doc::NodeId{30U});
    KEYER_CHECK(selected.value().records[1U].channel == doc::NodeId{31U});
}

void identity_is_canonical_and_active_malformed_payloads_fail_closed() {
    doc::Node bypassed_chroma = chroma_node(20U, 0.0);
    bypassed_chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_color_r}, std::string{"stale"});
    bypassed_chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_morphology_px},
        std::numeric_limits<std::int64_t>::max());
    doc::Node bypassed_luma = luma_node(21U, 0.0);
    bypassed_luma.properties.insert_or_assign(
        std::string{schema::luma_key_low_threshold}, 0.9);
    bypassed_luma.properties.insert_or_assign(
        std::string{schema::luma_key_high_threshold}, 0.1);

    doc::Root identity;
    identity = identity.set(node(1U, schema::stage_type));
    identity = identity.set(
        node(10U, schema::object_type, {whole(schema::scene_id, 1)}));
    identity = identity.set(bypassed_chroma);
    identity = identity.set(bypassed_luma);
    scene::SceneModel identity_model;
    identity_model.reset(identity);
    const auto authored = identity_model.effects();
    KEYER_CHECK(authored.size() == 2U);
    KEYER_CHECK(authored[0U].chroma_key ==
                (scene::ChromaKeyParameters{
                    {0.0F, 1.0F, 0.0F}, 0.1F, 0.1F, 0, 0.0F, 0.0F}));
    KEYER_CHECK(authored[1U].luma_key ==
                (scene::LumaKeyParameters{
                    0.0F, 1.0F, 0.0F, 0.0F, false, 0.0F}));
    KEYER_CHECK(evaluate(identity_model)
                    .instances.front()
                    .style.effects.spatial_operations.empty());
    KEYER_CHECK(scene::chroma_key_bounds(authored[0U].chroma_key) ==
                scene::EffectBoundsExpansion{});
    KEYER_CHECK(scene::luma_key_bounds(authored[1U].luma_key) ==
                scene::EffectBoundsExpansion{});

    doc::Node invalid_chroma = bypassed_chroma;
    invalid_chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_mix}, 1.0);
    doc::Node invalid_luma = bypassed_luma;
    invalid_luma.properties.insert_or_assign(
        std::string{schema::luma_key_mix}, 1.0);
    scene::SceneModel invalid_model;
    invalid_model.reset(identity.set(invalid_chroma).set(invalid_luma));
    KEYER_CHECK(invalid_model.effects().empty());
    KEYER_CHECK(invalid_model.counters().rejected >= 2U);
}

void keyer_state_participates_in_scene_digest() {
    scene::SceneModel model;
    model.reset(keyer_root(false));
    const scene::Scene baseline = evaluate(model);
    scene::Scene changed = baseline;
    changed.effects[0U].chroma_key.spill_suppression = 0.25F;
    KEYER_CHECK(scene::digest(changed) != scene::digest(baseline));
    changed = baseline;
    changed.instances.front()
        .style.effects.spatial_operations[1U]
        .luma_key.invert = false;
    KEYER_CHECK(scene::digest(changed) != scene::digest(baseline));
}

void director_undo_restores_both_typed_keyers_as_one_transaction() {
    const doc::Root original = keyer_root(false);
    const doc::Node original_chroma = *original.get(doc::NodeId{20U});
    const doc::Node original_luma = *original.get(doc::NodeId{21U});
    const doc::Node changed_chroma = chroma_node(20U, 0.1);
    const doc::Node changed_luma = luma_node(21U, 0.2);

    cmd::DirectorScheduler director{original};
    const cmd::UndoContextId context{1U};
    const cmd::TransactionId gesture{7U};
    KEYER_CHECK(director.schedule_edit(
        cmd::PutNode{changed_chroma}, cmd::PutNode{original_chroma}, context,
        gesture, cmd::FrameNumber{1U}));
    KEYER_CHECK(director.schedule_edit(
        cmd::PutNode{changed_luma}, cmd::PutNode{original_luma}, context,
        gesture, cmd::FrameNumber{1U}));
    KEYER_CHECK(director.undo_depth(context) == 1U);
    KEYER_CHECK(director.seal_frame(cmd::FrameNumber{1U}));
    KEYER_CHECK(director.schedule_undo(
        context, cmd::TransactionId{8U}, cmd::FrameNumber{2U}));
    KEYER_CHECK(*director.projected_document().get(doc::NodeId{20U}) ==
                original_chroma);
    KEYER_CHECK(*director.projected_document().get(doc::NodeId{21U}) ==
                original_luma);

    scene::SceneModel restored;
    restored.reset(director.projected_document());
    KEYER_CHECK(restored.effects()[0U].chroma_key.mix == 0.8F);
    KEYER_CHECK(restored.effects()[1U].luma_key.mix == 0.9F);
}

void project_save_load_and_incremental_projection_round_trip_exactly() {
    const doc::Root root = keyer_root();
    scene::SceneModel checkpointed;
    checkpointed.reset(root);

    scene::SceneModel streamed;
    doc::Root running;
    for (const core::u64 id : {31U, 30U, 21U, 20U, 10U, 1U}) {
        const doc::Root next = running.set(*root.get(doc::NodeId{id}));
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    KEYER_CHECK(streamed.effects() == checkpointed.effects());
    KEYER_CHECK(scene::digest(evaluate(streamed)) ==
                scene::digest(evaluate(checkpointed)));

    TempDirectory temporary;
    KEYER_CHECK(persist::save_project_file(temporary.file(), root));
    const auto loaded = persist::load_project_file(temporary.file());
    KEYER_CHECK(loaded.has_value());
    KEYER_CHECK(same_document(loaded.value(), root));
    scene::SceneModel restored;
    restored.reset(loaded.value());
    KEYER_CHECK(restored.effects() == checkpointed.effects());
    KEYER_CHECK(scene::digest(evaluate(restored)) ==
                scene::digest(evaluate(checkpointed)));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"typed projection and primary channels preserve order",
     &typed_projection_and_primary_channels_preserve_order},
    {"identity is canonical and active malformed payloads fail closed",
     &identity_is_canonical_and_active_malformed_payloads_fail_closed},
    {"keyer state participates in scene digest",
     &keyer_state_participates_in_scene_digest},
    {"Director undo restores both typed keyers as one transaction",
     &director_undo_restores_both_typed_keyers_as_one_transaction},
    {"project save load and incremental projection round trip exactly",
     &project_save_load_and_incremental_projection_round_trip_exactly},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
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
