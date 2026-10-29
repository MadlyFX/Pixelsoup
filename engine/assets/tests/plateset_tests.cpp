#include <mediaserver/assets/plateset.hpp>

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/library_edit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace schema = mediaserver::assets::schema;
using assets::PlatesetSlot;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PLATESET_CHECK(...)                                                    \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Plate final {
    std::uint64_t id{};
    std::string name;
    std::int64_t width{3'840};
    std::int64_t height{2'160};
    std::int64_t frames{2'880};
    std::int64_t rate_numerator{24};
    std::int64_t rate_denominator{1};
    std::string timecode;
};

[[nodiscard]] doc::Node media_node(const Plate& plate) {
    doc::Node node{};
    node.id = doc::NodeId{plate.id};
    node.type = std::string{schema::media_type};
    const auto put = [&node](std::string_view name, doc::PropertyValue value) {
        node.properties.insert_or_assign(std::string{name}, std::move(value));
    };
    put(schema::display_name, plate.name);
    put(schema::source, std::string{"D:\\plates\\"} + plate.name);
    put(schema::media_width, plate.width);
    put(schema::media_height, plate.height);
    put(schema::media_frame_count, plate.frames);
    put(schema::media_rate_numerator, plate.rate_numerator);
    put(schema::media_rate_denominator, plate.rate_denominator);
    if (!plate.timecode.empty()) {
        put(schema::media_start_timecode, plate.timecode);
    }
    return node;
}

[[nodiscard]] doc::Root with_plates(const std::vector<Plate>& plates) {
    doc::Root root;
    for (const Plate& plate : plates) {
        root = root.set(media_node(plate));
    }
    return root;
}

[[nodiscard]] assets::AssetRegistry evaluate(const doc::Root& root) {
    assets::AssetModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] const assets::PlatesetAsset*
plateset_in(const assets::AssetRegistry& registry, std::uint64_t id) {
    const assets::Asset* const asset = registry.asset(doc::NodeId{id});
    return asset != nullptr ? std::get_if<assets::PlatesetAsset>(asset)
                            : nullptr;
}

[[nodiscard]] std::size_t index(PlatesetSlot slot) {
    return static_cast<std::size_t>(slot);
}

// The array these were exported from names its seven cameras after the take,
// and a position tag is the last thing before the reel suffix.
void driving_array_names_propose_their_slots() {
    const auto slot = [](std::string_view name) {
        return assets::propose_plateset_slot(name);
    };
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-FC_1.mp4") ==
                   PlatesetSlot::Front);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-FL_1.mp4") ==
                   PlatesetSlot::FrontLeft);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-FR_1.mp4") ==
                   PlatesetSlot::FrontRight);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-SL_1.mp4") ==
                   PlatesetSlot::Left);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-BC_1.mp4") ==
                   PlatesetSlot::Back);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-BL_1.mp4") ==
                   PlatesetSlot::BackLeft);
    PLATESET_CHECK(slot("DP230703-042-Chicago, Illinois-BR_1.mp4") ==
                   PlatesetSlot::BackRight);
    // Spelled-out positions, alone and as a pair across a separator.
    PLATESET_CHECK(slot("take3_REAR_RIGHT.mov") == PlatesetSlot::BackRight);
    PLATESET_CHECK(slot("take3 front left.mov") == PlatesetSlot::FrontLeft);
    PLATESET_CHECK(slot("SIDE-RIGHT.mxf") == PlatesetSlot::Right);
    PLATESET_CHECK(slot("roof_cam.mp4") == PlatesetSlot::Ceiling);
    // The last tag wins; a folder or extension never decides.
    PLATESET_CHECK(slot("FC_retake_BL.mp4") == PlatesetSlot::BackLeft);
    PLATESET_CHECK(slot("D:\\FRONT\\clip_007.mp4") == std::nullopt);
    PLATESET_CHECK(slot("clip_007.FC") == std::nullopt);
    // Single letters and camera letters are never positions.
    PLATESET_CHECK(slot("A_CAM_B_001.mp4") == std::nullopt);
}

void proposals_fill_each_slot_once_and_pick_the_smallest_rig() {
    const assets::MediaAsset front{doc::NodeId{11U}, "front", "D:\\a\\FRONT.mp4"};
    const assets::MediaAsset left{doc::NodeId{12U}, "left", "D:\\a\\LEFT.mp4"};
    const assets::MediaAsset right{doc::NodeId{13U}, "right", "D:\\a\\RIGHT.mp4"};
    const assets::MediaAsset again{doc::NodeId{14U}, "front 2",
                                   "D:\\a\\FRONT_2.mp4"};
    const assets::MediaAsset loose{doc::NodeId{15U}, "slate", "D:\\a\\slate.mp4"};
    // Named by display name when the file says nothing.
    const assets::MediaAsset top{doc::NodeId{16U}, "TOP", "D:\\a\\c0009.mp4"};
    const std::array<const assets::MediaAsset*, 5U> three{&front, &left, &right,
                                                         &again, &loose};
    const assets::PlatesetProposal small = assets::propose_plateset(three);
    PLATESET_CHECK(small.slots[index(PlatesetSlot::Front)] == doc::NodeId{11U});
    PLATESET_CHECK(small.slots[index(PlatesetSlot::Left)] == doc::NodeId{12U});
    PLATESET_CHECK(small.slots[index(PlatesetSlot::Right)] == doc::NodeId{13U});
    PLATESET_CHECK((small.unplaced ==
                    std::vector<doc::NodeId>{doc::NodeId{14U}, doc::NodeId{15U}}));
    PLATESET_CHECK(small.rig == assets::PlatesetRig::Three);

    const std::array<const assets::MediaAsset*, 4U> five{&front, &left, &right,
                                                        &top};
    const assets::PlatesetProposal medium = assets::propose_plateset(five);
    PLATESET_CHECK(medium.slots[index(PlatesetSlot::Ceiling)] ==
                   doc::NodeId{16U});
    PLATESET_CHECK(medium.rig == assets::PlatesetRig::Five);
}

[[nodiscard]] assets::LibraryEdit create_edit(std::string name,
                                              assets::PlatesetRig rig) {
    assets::LibraryEdit edit{};
    edit.kind = assets::LibraryEdit::Kind::CreatePlateset;
    edit.name = std::move(name);
    edit.rig = rig;
    return edit;
}

void created_platesets_evaluate_with_their_slots() {
    doc::Root root = with_plates({{21U, "front.mp4"}, {22U, "left.mp4"}});
    assets::LibraryEdit create = create_edit("Chicago 042", assets::PlatesetRig::Five);
    create.slots[index(PlatesetSlot::Front)] = doc::NodeId{21U};
    create.slots[index(PlatesetSlot::Left)] = doc::NodeId{22U};
    // A slot naming something that is not media is not written.
    create.slots[index(PlatesetSlot::Back)] = doc::NodeId{99U};
    const auto created =
        assets::plan_library_edit(root, create, doc::NodeId{40U});
    PLATESET_CHECK(created.has_value());
    // A second, empty set: platesets have no source, so neither is a
    // duplicate of the other.
    const auto second = assets::plan_library_edit(
        *created, create_edit("Chicago 043", assets::PlatesetRig::Nine),
        doc::NodeId{41U});
    PLATESET_CHECK(second.has_value());

    assets::AssetModel model;
    model.reset(*second);
    const assets::AssetRegistry registry = model.evaluate();
    PLATESET_CHECK(model.counters().platesets == 2U);
    PLATESET_CHECK(model.counters().duplicate_sources == 0U);
    const assets::PlatesetAsset* const first = plateset_in(registry, 40U);
    PLATESET_CHECK(first != nullptr);
    PLATESET_CHECK(first->display_name == "Chicago 042");
    PLATESET_CHECK(first->rig == assets::PlatesetRig::Five);
    PLATESET_CHECK(first->slot(PlatesetSlot::Front).media == doc::NodeId{21U});
    PLATESET_CHECK(first->slot(PlatesetSlot::Left).media == doc::NodeId{22U});
    PLATESET_CHECK(first->slot(PlatesetSlot::Back).media == doc::NodeId{});
    PLATESET_CHECK(assets::asset_kind(*registry.asset(doc::NodeId{40U})) ==
                   assets::AssetKind::Plateset);
    PLATESET_CHECK(assets::asset_source(*registry.asset(doc::NodeId{40U})).empty());
    PLATESET_CHECK(plateset_in(registry, 41U) != nullptr);
}

void malformed_platesets_are_rejected() {
    const auto rejected = [](std::string_view property, doc::PropertyValue value) {
        doc::Node node{};
        node.id = doc::NodeId{50U};
        node.type = std::string{schema::plateset_type};
        node.properties.emplace(std::string{schema::display_name},
                                doc::PropertyValue{std::string{"Take"}});
        node.properties.insert_or_assign(std::string{property}, std::move(value));
        assets::AssetModel model;
        model.reset(doc::Root{}.set(node));
        return model.evaluate().asset(doc::NodeId{50U}) == nullptr &&
            model.counters().rejected == 1U;
    };
    PLATESET_CHECK(rejected("slot_front_offset", doc::PropertyValue{std::int64_t{-3}}));
    PLATESET_CHECK(rejected("slot_left", doc::PropertyValue{std::int64_t{50}}));
    PLATESET_CHECK(rejected("slot_left", doc::PropertyValue{std::string{"7"}}));
    PLATESET_CHECK(rejected(schema::plateset_rig,
                            doc::PropertyValue{std::string{"twelve"}}));
    PLATESET_CHECK(rejected(schema::display_name,
                            doc::PropertyValue{std::string{}}));
}

void slot_edits_move_plates_and_reset_offsets() {
    doc::Root root = with_plates({{61U, "a.mp4"}, {62U, "b.mp4"}});
    assets::LibraryEdit create = create_edit("Take", assets::PlatesetRig::Nine);
    create.slots[index(PlatesetSlot::Front)] = doc::NodeId{61U};
    create.slots[index(PlatesetSlot::Left)] = doc::NodeId{62U};
    root = *assets::plan_library_edit(root, create, doc::NodeId{70U});

    assets::LibraryEdit offsets{};
    offsets.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
    offsets.target = doc::NodeId{70U};
    offsets.offsets[index(PlatesetSlot::Front)] = 7;
    offsets.offsets[index(PlatesetSlot::Left)] = 13;
    // An offset on an empty slot is not written.
    offsets.offsets[index(PlatesetSlot::Ceiling)] = 5;
    root = *assets::plan_library_edit(root, offsets);
    {
        const assets::AssetRegistry registry = evaluate(root);
        const assets::PlatesetAsset* const take = plateset_in(registry, 70U);
        PLATESET_CHECK(take->slot(PlatesetSlot::Front).offset == 7);
        PLATESET_CHECK(take->slot(PlatesetSlot::Left).offset == 13);
        PLATESET_CHECK(take->slot(PlatesetSlot::Ceiling).offset == 0);
        PLATESET_CHECK(!root.get(doc::NodeId{70U})->properties.contains(
            "slot_ceiling_offset"));
    }
    // The left plate moves to the right slot: it leaves the left empty, and
    // the right starts without the offset it had on the left.
    assets::LibraryEdit move{};
    move.kind = assets::LibraryEdit::Kind::SetPlatesetSlot;
    move.target = doc::NodeId{70U};
    move.slot = PlatesetSlot::Right;
    move.media = doc::NodeId{62U};
    root = *assets::plan_library_edit(root, move);
    {
        const assets::AssetRegistry registry = evaluate(root);
        const assets::PlatesetAsset* const take = plateset_in(registry, 70U);
        PLATESET_CHECK(take->slot(PlatesetSlot::Left).media == doc::NodeId{});
        PLATESET_CHECK(take->slot(PlatesetSlot::Left).offset == 0);
        PLATESET_CHECK(take->slot(PlatesetSlot::Right).media == doc::NodeId{62U});
        PLATESET_CHECK(take->slot(PlatesetSlot::Right).offset == 0);
        PLATESET_CHECK(take->slot(PlatesetSlot::Front).offset == 7);
    }
    // Re-assigning the same plate changes nothing; a non-media id is refused.
    PLATESET_CHECK(!assets::plan_library_edit(root, move).has_value());
    move.media = doc::NodeId{70U};
    PLATESET_CHECK(!assets::plan_library_edit(root, move).has_value());
    // Clearing empties the slot.
    move.media = {};
    move.slot = PlatesetSlot::Front;
    root = *assets::plan_library_edit(root, move);
    PLATESET_CHECK(plateset_in(evaluate(root), 70U)
                       ->slot(PlatesetSlot::Front)
                       .media == doc::NodeId{});

    assets::LibraryEdit rig{};
    rig.kind = assets::LibraryEdit::Kind::SetPlatesetRig;
    rig.target = doc::NodeId{70U};
    rig.rig = assets::PlatesetRig::Three;
    root = *assets::plan_library_edit(root, rig);
    PLATESET_CHECK(plateset_in(evaluate(root), 70U)->rig ==
                   assets::PlatesetRig::Three);

    // Platesets rename and file into folders like any other asset.
    assets::LibraryEdit rename{};
    rename.kind = assets::LibraryEdit::Kind::Rename;
    rename.target = doc::NodeId{70U};
    rename.name = "Night run";
    root = *assets::plan_library_edit(root, rename);
    PLATESET_CHECK(plateset_in(evaluate(root), 70U)->display_name == "Night run");
}

[[nodiscard]] assets::PlatesetAsset set_of(
    std::initializer_list<std::pair<PlatesetSlot, std::uint64_t>> slots) {
    assets::PlatesetAsset plateset{};
    plateset.id = doc::NodeId{90U};
    plateset.display_name = "Take";
    for (const auto& [slot, media] : slots) {
        plateset.slots[index(slot)].media = doc::NodeId{media};
    }
    return plateset;
}

[[nodiscard]] bool has(const assets::PlatesetReadiness& readiness,
                       assets::PlatesetIssueKind kind,
                       std::optional<PlatesetSlot> slot = std::nullopt) {
    for (const assets::PlatesetIssue& issue : readiness.issues) {
        if (issue.kind == kind && (!slot.has_value() || issue.slot == slot)) {
            return true;
        }
    }
    return false;
}

void readiness_names_each_blocking_problem() {
    Plate still{103U, "still.png"};
    still.frames = 1;
    Plate fifty{104U, "fifty.mp4"};
    fifty.rate_numerator = 25;
    const assets::AssetRegistry registry =
        evaluate(with_plates({{101U, "front.mp4"}, {102U, "left.mp4"}, still,
                              fifty}));

    const assets::PlatesetReadiness empty =
        assets::evaluate_plateset(set_of({}), registry);
    PLATESET_CHECK(has(empty, assets::PlatesetIssueKind::Empty));
    PLATESET_CHECK(empty.issues.size() == 1U);
    PLATESET_CHECK(!empty.ready());

    const assets::PlatesetReadiness problems = assets::evaluate_plateset(
        set_of({{PlatesetSlot::Left, 102U},
                {PlatesetSlot::FrontLeft, 101U},
                {PlatesetSlot::Right, 103U},
                {PlatesetSlot::Back, 104U},
                {PlatesetSlot::Ceiling, 999U}}),
        registry);
    PLATESET_CHECK(has(problems, assets::PlatesetIssueKind::NoFront,
                       PlatesetSlot::Front));
    PLATESET_CHECK(has(problems, assets::PlatesetIssueKind::NotVideo,
                       PlatesetSlot::Right));
    PLATESET_CHECK(has(problems, assets::PlatesetIssueKind::FrameRateMismatch,
                       PlatesetSlot::Back));
    PLATESET_CHECK(has(problems, assets::PlatesetIssueKind::MissingMedia,
                       PlatesetSlot::Ceiling));
    PLATESET_CHECK(!has(problems, assets::PlatesetIssueKind::FrameRateMismatch,
                        PlatesetSlot::Left));
    PLATESET_CHECK(problems.filled == 5U);
    PLATESET_CHECK(!problems.rate.has_value());
    PLATESET_CHECK(!problems.ready());

    const assets::PlatesetReadiness good = assets::evaluate_plateset(
        set_of({{PlatesetSlot::Front, 101U}, {PlatesetSlot::Left, 102U}}),
        registry);
    PLATESET_CHECK(good.ready());
    PLATESET_CHECK(good.issues.empty());
    PLATESET_CHECK((good.rate == assets::MediaFrameRate{24U, 1U}));
    PLATESET_CHECK(good.common_frames == 2'880);
}

void offsets_shrink_the_common_span() {
    Plate shorter{112U, "left.mp4"};
    shorter.frames = 2'000;
    const assets::AssetRegistry registry =
        evaluate(with_plates({{111U, "front.mp4"}, shorter}));
    assets::PlatesetAsset take = set_of(
        {{PlatesetSlot::Front, 111U}, {PlatesetSlot::Left, 112U}});
    take.slots[index(PlatesetSlot::Front)].offset = 900;
    take.slots[index(PlatesetSlot::Left)].offset = 37;
    // Front covers 2880 - 900 = 1980, the left 2000 - 37 = 1963.
    PLATESET_CHECK(assets::evaluate_plateset(take, registry).common_frames ==
                   1'963);
    take.slots[index(PlatesetSlot::Left)].offset = 2'000;
    const assets::PlatesetReadiness none =
        assets::evaluate_plateset(take, registry);
    PLATESET_CHECK(has(none, assets::PlatesetIssueKind::NoCommonSpan));
    PLATESET_CHECK(none.common_frames == 0);
}

void decode_budget_counts_proxies_at_their_size() {
    std::vector<Plate> plates;
    for (std::uint64_t id = 121U; id < 128U; ++id) {
        plates.push_back(Plate{id, "plate" + std::to_string(id) + ".mp4"});
    }
    doc::Root root = with_plates(plates);
    assets::PlatesetAsset four = set_of({{PlatesetSlot::Front, 121U},
                                         {PlatesetSlot::Left, 122U},
                                         {PlatesetSlot::Right, 123U},
                                         {PlatesetSlot::Back, 124U}});
    // The measured reference load is exactly within budget.
    const assets::PlatesetReadiness at_budget =
        assets::evaluate_plateset(four, evaluate(root));
    PLATESET_CHECK(at_budget.decode_pixels_per_second ==
                   4ULL * 3'840ULL * 2'160ULL * 24ULL);
    PLATESET_CHECK(!has(at_budget, assets::PlatesetIssueKind::DecodeOverBudget));
    assets::PlatesetAsset seven = four;
    seven.slots[index(PlatesetSlot::FrontLeft)].media = doc::NodeId{125U};
    seven.slots[index(PlatesetSlot::FrontRight)].media = doc::NodeId{126U};
    seven.slots[index(PlatesetSlot::BackLeft)].media = doc::NodeId{127U};
    const assets::PlatesetReadiness over =
        assets::evaluate_plateset(seven, evaluate(root));
    PLATESET_CHECK(has(over, assets::PlatesetIssueKind::DecodeOverBudget));
    // Over budget is reported, not blocking.
    PLATESET_CHECK(over.ready());

    // Three plates preferring the 720p proxy decode at 1280x720.
    for (std::uint64_t id : {125U, 126U, 127U}) {
        doc::Node node = *root.get(doc::NodeId{id});
        node.properties.insert_or_assign(
            std::string{schema::requested_proxy_profile},
            doc::PropertyValue{std::string{"proxy-720p-cfr-h264"}});
        node.properties.insert_or_assign(
            std::string{schema::proxy_playback_policy},
            doc::PropertyValue{std::string{schema::proxy_policy_prefer}});
        root = root.set(std::move(node));
    }
    const assets::AssetRegistry proxied = evaluate(root);
    PLATESET_CHECK(std::get<assets::MediaAsset>(*proxied.asset(doc::NodeId{125U}))
                       .requested_proxy_profile.has_value());
    const assets::PlatesetReadiness relieved =
        assets::evaluate_plateset(seven, proxied);
    PLATESET_CHECK(relieved.decode_pixels_per_second ==
                   4ULL * 3'840ULL * 2'160ULL * 24ULL +
                       3ULL * 1'280ULL * 720ULL * 24ULL);
    PLATESET_CHECK(has(relieved, assets::PlatesetIssueKind::DecodeOverBudget));
}

void timecode_sync_offsets_the_early_starters() {
    Plate front{131U, "front.mp4"};
    front.timecode = "01:00:00:10";
    Plate left{132U, "left.mp4"};
    left.timecode = "01:00:02:00";
    Plate right{133U, "right.mp4"};
    right.timecode = "00:59:59:23";
    const assets::AssetRegistry registry =
        evaluate(with_plates({front, left, right}));
    const assets::PlatesetTimecodeResult result = assets::plateset_timecode_sync(
        set_of({{PlatesetSlot::Front, 131U},
                {PlatesetSlot::Left, 132U},
                {PlatesetSlot::Right, 133U}}),
        registry);
    PLATESET_CHECK(result.sync.has_value());
    // The left plate rolled last; the front rolled 38 frames before it and
    // the right 49.
    PLATESET_CHECK(result.sync->offsets[index(PlatesetSlot::Left)] == 0);
    PLATESET_CHECK(result.sync->offsets[index(PlatesetSlot::Front)] == 38);
    PLATESET_CHECK(result.sync->offsets[index(PlatesetSlot::Right)] == 49);
    PLATESET_CHECK(result.sync->offsets[index(PlatesetSlot::Back)] == 0);
}

void timecode_sync_refuses_what_it_cannot_line_up() {
    Plate front{141U, "front.mp4"};
    front.timecode = "01:00:00:00";
    Plate bare{142U, "left.mp4"};
    Plate fifty{143U, "right.mp4"};
    fifty.rate_numerator = 25;
    fifty.timecode = "01:00:00:00";
    Plate ntsc_drop{144U, "back.mp4"};
    ntsc_drop.rate_numerator = 30'000;
    ntsc_drop.rate_denominator = 1'001;
    ntsc_drop.timecode = "01:00:00;00";
    Plate ntsc{145U, "ceiling.mp4"};
    ntsc.rate_numerator = 30'000;
    ntsc.rate_denominator = 1'001;
    ntsc.timecode = "01:00:00:00";
    const assets::AssetRegistry registry =
        evaluate(with_plates({front, bare, fifty, ntsc_drop, ntsc}));

    const auto missing = assets::plateset_timecode_sync(
        set_of({{PlatesetSlot::Front, 141U}, {PlatesetSlot::Left, 142U}}),
        registry);
    PLATESET_CHECK(!missing.sync.has_value());
    PLATESET_CHECK(missing.error == assets::PlatesetTimecodeError::MissingTimecode);
    PLATESET_CHECK((missing.slots == std::vector<PlatesetSlot>{PlatesetSlot::Left}));
    PLATESET_CHECK(assets::plateset_timecode_error_text(missing).find("LEFT") !=
                   std::string::npos);

    const auto rates = assets::plateset_timecode_sync(
        set_of({{PlatesetSlot::Front, 141U}, {PlatesetSlot::Right, 143U}}),
        registry);
    PLATESET_CHECK(rates.error == assets::PlatesetTimecodeError::FrameRateMismatch);
    PLATESET_CHECK((rates.slots == std::vector<PlatesetSlot>{PlatesetSlot::Right}));

    const auto drop = assets::plateset_timecode_sync(
        set_of({{PlatesetSlot::Back, 144U}, {PlatesetSlot::Ceiling, 145U}}),
        registry);
    PLATESET_CHECK(drop.error == assets::PlatesetTimecodeError::DropFrameMismatch);
    PLATESET_CHECK((drop.slots ==
                    std::vector<PlatesetSlot>{PlatesetSlot::Ceiling}));
}

void drop_frame_timecode_counts_real_frames() {
    const assets::MediaFrameRate ntsc{30'000U, 1'001U};
    // An hour of 29.97 drop-frame is 107,892 frames, not 108,000.
    PLATESET_CHECK(assets::timecode_frames("01:00:00;00", ntsc) == 107'892);
    PLATESET_CHECK(assets::timecode_frames("01:00:00:00", ntsc) == 108'000);
    PLATESET_CHECK(assets::timecode_text(107'892, ntsc, true) == "01:00:00;00");
    PLATESET_CHECK(assets::timecode_frames("00:00:01:25", {25U, 1U}) ==
                   std::nullopt);
    PLATESET_CHECK(assets::timecode_is_drop_frame("00:00:00.00"));
}

void the_digest_tracks_plateset_offsets() {
    doc::Root root = with_plates({{151U, "front.mp4"}});
    assets::LibraryEdit create = create_edit("Take", assets::PlatesetRig::Nine);
    create.slots[index(PlatesetSlot::Front)] = doc::NodeId{151U};
    root = *assets::plan_library_edit(root, create, doc::NodeId{160U});
    const core::u64 before = assets::digest(evaluate(root));
    assets::LibraryEdit offsets{};
    offsets.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
    offsets.target = doc::NodeId{160U};
    offsets.offsets[index(PlatesetSlot::Front)] = 3;
    root = *assets::plan_library_edit(root, offsets);
    PLATESET_CHECK(assets::digest(evaluate(root)) != before);
}

// Kept moments live on the set, in front-plate frame order, and the half
// size flag with them; both are authored, so both change the digest.
void moments_and_half_resolution_are_kept_on_the_set() {
    doc::Root root = with_plates({{171U, "front.mp4"}});
    assets::LibraryEdit create = create_edit("Take", assets::PlatesetRig::Nine);
    create.slots[index(PlatesetSlot::Front)] = doc::NodeId{171U};
    root = *assets::plan_library_edit(root, create, doc::NodeId{180U});
    const core::u64 plain = assets::digest(evaluate(root));

    assets::LibraryEdit moments{};
    moments.kind = assets::LibraryEdit::Kind::SetPlatesetMoments;
    moments.target = doc::NodeId{180U};
    moments.moments = {{412, assets::PlatesetMomentKind::TurningRight},
                       {53, assets::PlatesetMomentKind::IntoShade}};
    root = *assets::plan_library_edit(root, moments);
    const assets::AssetRegistry registry = evaluate(root);
    const assets::PlatesetAsset* take = plateset_in(registry, 180U);
    PLATESET_CHECK(take->moments.size() == 2U);
    PLATESET_CHECK(take->moments[0].frame == 53);
    PLATESET_CHECK(take->moments[0].kind ==
                   assets::PlatesetMomentKind::IntoShade);
    PLATESET_CHECK(take->moments[1].kind ==
                   assets::PlatesetMomentKind::TurningRight);
    PLATESET_CHECK(std::get<std::string>(
                       root.get(doc::NodeId{180U})
                           ->properties.at("moment_1_kind")) ==
                   "turning_right");
    const core::u64 with_moments = assets::digest(evaluate(root));
    PLATESET_CHECK(with_moments != plain);

    // Keeping fewer drops the rest; the numbering has no gaps.
    moments.moments = {{412, assets::PlatesetMomentKind::TurningRight}};
    root = *assets::plan_library_edit(root, moments);
    PLATESET_CHECK(plateset_in(evaluate(root), 180U)->moments.size() == 1U);
    PLATESET_CHECK(!root.get(doc::NodeId{180U})->properties.contains(
        "moment_1_frame"));

    assets::LibraryEdit half{};
    half.kind = assets::LibraryEdit::Kind::SetPlatesetHalfResolution;
    half.target = doc::NodeId{180U};
    half.half = true;
    root = *assets::plan_library_edit(root, half);
    PLATESET_CHECK(plateset_in(evaluate(root), 180U)->half_resolution);
    PLATESET_CHECK(!assets::plan_library_edit(root, half).has_value());
    half.half = false;
    root = *assets::plan_library_edit(root, half);
    PLATESET_CHECK(!plateset_in(evaluate(root), 180U)->half_resolution);
}

struct Case final {
    const char* name;
    void (*function)();
};

const std::array cases{
    Case{"driving array names propose their slots",
         &driving_array_names_propose_their_slots},
    Case{"proposals fill each slot once and pick the smallest rig",
         &proposals_fill_each_slot_once_and_pick_the_smallest_rig},
    Case{"created platesets evaluate with their slots",
         &created_platesets_evaluate_with_their_slots},
    Case{"malformed platesets are rejected", &malformed_platesets_are_rejected},
    Case{"slot edits move plates and reset offsets",
         &slot_edits_move_plates_and_reset_offsets},
    Case{"readiness names each blocking problem",
         &readiness_names_each_blocking_problem},
    Case{"offsets shrink the common span", &offsets_shrink_the_common_span},
    Case{"decode budget counts proxies at their size",
         &decode_budget_counts_proxies_at_their_size},
    Case{"timecode sync offsets the early starters",
         &timecode_sync_offsets_the_early_starters},
    Case{"timecode sync refuses what it cannot line up",
         &timecode_sync_refuses_what_it_cannot_line_up},
    Case{"drop-frame timecode counts real frames",
         &drop_frame_timecode_counts_real_frames},
    Case{"the digest tracks plateset offsets",
         &the_digest_tracks_plateset_offsets},
    Case{"moments and half resolution are kept on the set",
         &moments_and_half_resolution_are_kept_on_the_set},
};

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
