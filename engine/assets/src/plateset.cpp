#include <mediaserver/assets/plateset.hpp>

#include <mediaserver/assets/asset_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mediaserver::assets {
namespace {

constexpr std::array<PlatesetSlot, 9U> nine_slots = all_plateset_slots;
constexpr std::array<PlatesetSlot, 5U> five_slots{
    PlatesetSlot::Front, PlatesetSlot::Right, PlatesetSlot::Back,
    PlatesetSlot::Left, PlatesetSlot::Ceiling};
constexpr std::array<PlatesetSlot, 3U> three_slots{
    PlatesetSlot::Front, PlatesetSlot::Right, PlatesetSlot::Left};

[[nodiscard]] core::u32 nominal_rate(MediaFrameRate rate) noexcept {
    if (rate.denominator == 0U || rate.numerator == 0U) {
        return 0U;
    }
    return static_cast<core::u32>(
        std::llround(static_cast<double>(rate.numerator) /
                     static_cast<double>(rate.denominator)));
}

[[nodiscard]] bool drop_frame_rate(core::u32 nominal) noexcept {
    return nominal != 0U && nominal % 30U == 0U;
}

[[nodiscard]] bool same_rate(MediaFrameRate left,
                             MediaFrameRate right) noexcept {
    return static_cast<core::u64>(left.numerator) * right.denominator ==
        static_cast<core::u64>(right.numerator) * left.denominator;
}

[[nodiscard]] char upper(char character) noexcept {
    return character >= 'a' && character <= 'z'
        ? static_cast<char>(character - 'a' + 'A')
        : character;
}

[[nodiscard]] bool alphanumeric(char character) noexcept {
    return (character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z');
}

[[nodiscard]] std::optional<PlatesetSlot> single_tag(std::string_view tag) {
    struct Tag final {
        std::string_view text;
        PlatesetSlot slot;
    };
    static constexpr std::array<Tag, 31U> tags{{
        {"FC", PlatesetSlot::Front},          {"FRONT", PlatesetSlot::Front},
        {"FWD", PlatesetSlot::Front},         {"FORWARD", PlatesetSlot::Front},
        {"FL", PlatesetSlot::FrontLeft},      {"FRONTLEFT", PlatesetSlot::FrontLeft},
        {"FR", PlatesetSlot::FrontRight},     {"FRONTRIGHT", PlatesetSlot::FrontRight},
        {"SL", PlatesetSlot::Left},           {"LEFT", PlatesetSlot::Left},
        {"SR", PlatesetSlot::Right},          {"RIGHT", PlatesetSlot::Right},
        {"BC", PlatesetSlot::Back},           {"RC", PlatesetSlot::Back},
        {"BACK", PlatesetSlot::Back},         {"REAR", PlatesetSlot::Back},
        {"BL", PlatesetSlot::BackLeft},       {"RL", PlatesetSlot::BackLeft},
        {"BACKLEFT", PlatesetSlot::BackLeft}, {"REARLEFT", PlatesetSlot::BackLeft},
        {"BR", PlatesetSlot::BackRight},      {"RR", PlatesetSlot::BackRight},
        {"BACKRIGHT", PlatesetSlot::BackRight},
        {"REARRIGHT", PlatesetSlot::BackRight},
        {"TOP", PlatesetSlot::Ceiling},       {"CEILING", PlatesetSlot::Ceiling},
        {"ROOF", PlatesetSlot::Ceiling},      {"SKY", PlatesetSlot::Ceiling},
        {"UP", PlatesetSlot::Ceiling},        {"OVERHEAD", PlatesetSlot::Ceiling},
        {"REFLECTION", PlatesetSlot::Ceiling},
    }};
    for (const Tag& candidate : tags) {
        if (candidate.text == tag) {
            return candidate.slot;
        }
    }
    return std::nullopt;
}

// Two tags that name one position between them (`FRONT_LEFT`, `REAR RIGHT`,
// `SIDE_LEFT`).
[[nodiscard]] std::optional<PlatesetSlot> paired_tags(std::string_view first,
                                                      std::string_view second) {
    const bool left = second == "LEFT";
    const bool right = second == "RIGHT";
    if (!left && !right) {
        return std::nullopt;
    }
    if (first == "FRONT") {
        return left ? PlatesetSlot::FrontLeft : PlatesetSlot::FrontRight;
    }
    if (first == "BACK" || first == "REAR") {
        return left ? PlatesetSlot::BackLeft : PlatesetSlot::BackRight;
    }
    if (first == "SIDE") {
        return left ? PlatesetSlot::Left : PlatesetSlot::Right;
    }
    return std::nullopt;
}

[[nodiscard]] core::u64 pixel_rate(core::u64 width, core::u64 height,
                                   MediaFrameRate rate) noexcept {
    if (rate.denominator == 0U) {
        return 0U;
    }
    return width * height * rate.numerator / rate.denominator;
}

} // namespace

std::string_view plateset_slot_key(PlatesetSlot slot) noexcept {
    switch (slot) {
    case PlatesetSlot::Front:
        return "front";
    case PlatesetSlot::FrontRight:
        return "front_right";
    case PlatesetSlot::Right:
        return "right";
    case PlatesetSlot::BackRight:
        return "back_right";
    case PlatesetSlot::Back:
        return "back";
    case PlatesetSlot::BackLeft:
        return "back_left";
    case PlatesetSlot::Left:
        return "left";
    case PlatesetSlot::FrontLeft:
        return "front_left";
    case PlatesetSlot::Ceiling:
        return "ceiling";
    }
    return "front";
}

std::string_view plateset_slot_label(PlatesetSlot slot) noexcept {
    switch (slot) {
    case PlatesetSlot::Front:
        return "FRONT";
    case PlatesetSlot::FrontRight:
        return "FRONT RIGHT 3/4";
    case PlatesetSlot::Right:
        return "RIGHT";
    case PlatesetSlot::BackRight:
        return "BACK RIGHT 3/4";
    case PlatesetSlot::Back:
        return "BACK";
    case PlatesetSlot::BackLeft:
        return "BACK LEFT 3/4";
    case PlatesetSlot::Left:
        return "LEFT";
    case PlatesetSlot::FrontLeft:
        return "FRONT LEFT 3/4";
    case PlatesetSlot::Ceiling:
        return "CEILING REFLECTION";
    }
    return "FRONT";
}

std::string_view plateset_slot_tag(PlatesetSlot slot) noexcept {
    switch (slot) {
    case PlatesetSlot::Front:
        return "F";
    case PlatesetSlot::FrontRight:
        return "FR";
    case PlatesetSlot::Right:
        return "R";
    case PlatesetSlot::BackRight:
        return "BR";
    case PlatesetSlot::Back:
        return "B";
    case PlatesetSlot::BackLeft:
        return "BL";
    case PlatesetSlot::Left:
        return "L";
    case PlatesetSlot::FrontLeft:
        return "FL";
    case PlatesetSlot::Ceiling:
        return "TOP";
    }
    return "F";
}

std::string plateset_slot_property(PlatesetSlot slot) {
    std::string name{schema::plateset_slot_prefix};
    name += plateset_slot_key(slot);
    return name;
}

std::string plateset_offset_property(PlatesetSlot slot) {
    std::string name = plateset_slot_property(slot);
    name += schema::plateset_offset_suffix;
    return name;
}

std::string plateset_moment_property(std::size_t index,
                                     std::string_view field) {
    std::string name{schema::plateset_moment_prefix};
    name += std::to_string(index);
    name += '_';
    name += field;
    return name;
}

std::string_view plateset_moment_key(PlatesetMomentKind kind) noexcept {
    switch (kind) {
    case PlatesetMomentKind::Stopped:
        return "stopped";
    case PlatesetMomentKind::TurningLeft:
        return "turning_left";
    case PlatesetMomentKind::TurningRight:
        return "turning_right";
    case PlatesetMomentKind::Bump:
        return "bump";
    case PlatesetMomentKind::IntoShade:
        return "into_shade";
    }
    return "stopped";
}

std::optional<PlatesetMomentKind>
plateset_moment_kind_from_key(std::string_view key) noexcept {
    for (const PlatesetMomentKind kind : all_plateset_moment_kinds) {
        if (plateset_moment_key(kind) == key) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view plateset_moment_label(PlatesetMomentKind kind) noexcept {
    switch (kind) {
    case PlatesetMomentKind::Stopped:
        return "Stopped";
    case PlatesetMomentKind::TurningLeft:
        return "Turning left";
    case PlatesetMomentKind::TurningRight:
        return "Turning right";
    case PlatesetMomentKind::Bump:
        return "Bump";
    case PlatesetMomentKind::IntoShade:
        return "Into shade";
    }
    return "Stopped";
}

std::string_view plateset_moment_tag(PlatesetMomentKind kind) noexcept {
    switch (kind) {
    case PlatesetMomentKind::Stopped:
        return "Stop";
    case PlatesetMomentKind::TurningLeft:
        return "Left";
    case PlatesetMomentKind::TurningRight:
        return "Right";
    case PlatesetMomentKind::Bump:
        return "Bump";
    case PlatesetMomentKind::IntoShade:
        return "Shade";
    }
    return "Stop";
}

std::span<const PlatesetSlot> plateset_rig_slots(PlatesetRig rig) noexcept {
    switch (rig) {
    case PlatesetRig::Nine:
        return nine_slots;
    case PlatesetRig::Five:
        return five_slots;
    case PlatesetRig::Three:
        return three_slots;
    }
    return nine_slots;
}

std::string_view plateset_rig_key(PlatesetRig rig) noexcept {
    switch (rig) {
    case PlatesetRig::Nine:
        return schema::plateset_rig_nine;
    case PlatesetRig::Five:
        return schema::plateset_rig_five;
    case PlatesetRig::Three:
        return schema::plateset_rig_three;
    }
    return schema::plateset_rig_nine;
}

std::optional<PlatesetRig>
plateset_rig_from_key(std::string_view key) noexcept {
    if (key == schema::plateset_rig_nine) {
        return PlatesetRig::Nine;
    }
    if (key == schema::plateset_rig_five) {
        return PlatesetRig::Five;
    }
    if (key == schema::plateset_rig_three) {
        return PlatesetRig::Three;
    }
    return std::nullopt;
}

std::string_view plateset_rig_label(PlatesetRig rig) noexcept {
    switch (rig) {
    case PlatesetRig::Nine:
        return "9 camera";
    case PlatesetRig::Five:
        return "5 camera";
    case PlatesetRig::Three:
        return "3 camera";
    }
    return "9 camera";
}

bool timecode_is_drop_frame(std::string_view timecode) noexcept {
    return timecode.find(';') != std::string_view::npos ||
        timecode.find('.') != std::string_view::npos;
}

std::optional<core::i64> timecode_frames(std::string_view timecode,
                                         MediaFrameRate rate) {
    const core::u32 fps = nominal_rate(rate);
    if (fps == 0U) {
        return std::nullopt;
    }
    std::array<core::i64, 4U> fields{};
    std::size_t field = 0U;
    bool digits = false;
    for (const char character : timecode) {
        if (character >= '0' && character <= '9') {
            if (fields[field] > 1'000'000) {
                return std::nullopt;
            }
            fields[field] = fields[field] * 10 + (character - '0');
            digits = true;
        } else if (character == ':' || character == ';' || character == '.') {
            if (!digits || ++field >= fields.size()) {
                return std::nullopt;
            }
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (field != 3U || !digits) {
        return std::nullopt;
    }
    const auto [hours, minutes, seconds, frames] = fields;
    if (minutes >= 60 || seconds >= 60 ||
        frames >= static_cast<core::i64>(fps)) {
        return std::nullopt;
    }
    const core::i64 nominal = static_cast<core::i64>(fps);
    core::i64 total =
        ((hours * 60 + minutes) * 60 + seconds) * nominal + frames;
    if (timecode_is_drop_frame(timecode) && drop_frame_rate(fps)) {
        const core::i64 dropped = nominal / 15;
        const core::i64 total_minutes = hours * 60 + minutes;
        total -= dropped * (total_minutes - total_minutes / 10);
    }
    return total;
}

std::string timecode_text(core::i64 frame, MediaFrameRate rate,
                          bool drop_frame) {
    const core::u32 fps = nominal_rate(rate);
    if (fps == 0U) {
        return {};
    }
    const core::i64 nominal = static_cast<core::i64>(fps);
    const bool drop = drop_frame && drop_frame_rate(fps);
    // Timecode wraps at 24 hours; the frames in a real day at this rate.
    const core::i64 dropped = drop ? nominal / 15 : 0;
    const core::i64 per_ten_minutes = nominal * 600 - dropped * 9;
    const core::i64 day = per_ten_minutes * 6 * 24;
    frame %= day;
    if (frame < 0) {
        frame += day;
    }
    if (drop) {
        const core::i64 per_minute = nominal * 60 - dropped;
        const core::i64 tens = frame / per_ten_minutes;
        const core::i64 rest = frame % per_ten_minutes;
        frame += dropped * 9 * tens +
            (rest > dropped ? dropped * ((rest - dropped) / per_minute) : 0);
    }
    const core::i64 frames = frame % nominal;
    const core::i64 seconds = (frame / nominal) % 60;
    const core::i64 minutes = (frame / (nominal * 60)) % 60;
    const core::i64 hours = frame / (nominal * 3600);
    std::array<char, 32U> text{};
    const int length = std::snprintf(
        text.data(), text.size(), "%02lld:%02lld:%02lld%c%02lld",
        static_cast<long long>(hours), static_cast<long long>(minutes),
        static_cast<long long>(seconds), drop ? ';' : ':',
        static_cast<long long>(frames));
    return std::string{text.data(),
                       static_cast<std::size_t>(std::max(length, 0))};
}

std::optional<PlatesetSlot>
propose_plateset_slot(std::string_view name) noexcept {
    // The file name alone: a folder called `FRONT` says nothing about the
    // plate inside it, and an extension is never a position.
    if (const std::size_t slash = name.find_last_of("/\\");
        slash != std::string_view::npos) {
        name.remove_prefix(slash + 1U);
    }
    if (const std::size_t dot = name.rfind('.');
        dot != std::string_view::npos && dot > 0U) {
        name = name.substr(0U, dot);
    }
    std::array<std::string, 32U> tokens;
    std::size_t count = 0U;
    std::string current;
    const auto flush = [&] {
        if (!current.empty() && count < tokens.size()) {
            tokens[count++] = current;
        }
        current.clear();
    };
    for (const char character : name) {
        if (alphanumeric(character)) {
            current.push_back(upper(character));
        } else {
            flush();
        }
    }
    flush();

    std::optional<PlatesetSlot> found;
    for (std::size_t index = 0U; index < count; ++index) {
        if (index + 1U < count) {
            if (const auto pair = paired_tags(tokens[index], tokens[index + 1U]);
                pair.has_value()) {
                found = pair;
                ++index;
                continue;
            }
        }
        if (const auto single = single_tag(tokens[index]); single.has_value()) {
            found = single;
        }
    }
    return found;
}

PlatesetProposal propose_plateset(std::span<const MediaAsset* const> media) {
    PlatesetProposal proposal;
    for (const MediaAsset* const asset : media) {
        if (asset == nullptr) {
            continue;
        }
        std::optional<PlatesetSlot> slot =
            propose_plateset_slot(asset->source);
        if (!slot.has_value()) {
            slot = propose_plateset_slot(asset->display_name);
        }
        if (!slot.has_value() ||
            proposal.slots[static_cast<std::size_t>(*slot)] != doc::NodeId{}) {
            proposal.unplaced.push_back(asset->id);
            continue;
        }
        proposal.slots[static_cast<std::size_t>(*slot)] = asset->id;
    }
    const auto fits = [&proposal](PlatesetRig rig) {
        const std::span<const PlatesetSlot> offered = plateset_rig_slots(rig);
        for (const PlatesetSlot slot : all_plateset_slots) {
            if (proposal.slots[static_cast<std::size_t>(slot)] != doc::NodeId{} &&
                std::find(offered.begin(), offered.end(), slot) ==
                    offered.end()) {
                return false;
            }
        }
        return true;
    };
    proposal.rig = fits(PlatesetRig::Three)
        ? PlatesetRig::Three
        : (fits(PlatesetRig::Five) ? PlatesetRig::Five : PlatesetRig::Nine);
    return proposal;
}

bool PlatesetReadiness::ready() const noexcept {
    return std::none_of(issues.begin(), issues.end(),
                        [](const PlatesetIssue& issue) noexcept {
                            return issue.blocking();
                        });
}

PlatesetReadiness evaluate_plateset(const PlatesetAsset& plateset,
                                    const AssetRegistry& registry,
                                    core::u64 decode_budget) {
    PlatesetReadiness readiness;
    readiness.decode_budget = decode_budget;
    std::vector<std::pair<PlatesetSlot, const MediaAsset*>> videos;
    for (const PlatesetSlot slot : all_plateset_slots) {
        const PlatesetSlotAssignment& assignment = plateset.slot(slot);
        if (assignment.media == doc::NodeId{}) {
            continue;
        }
        ++readiness.filled;
        const Asset* const asset = registry.asset(assignment.media);
        const auto* const media =
            asset != nullptr ? std::get_if<MediaAsset>(asset) : nullptr;
        if (media == nullptr) {
            readiness.issues.push_back(
                PlatesetIssue{PlatesetIssueKind::MissingMedia, slot});
            continue;
        }
        if (!media->frame_rate.has_value() || !media->frame_count.has_value() ||
            *media->frame_count <= 1U || media->frame_rate->numerator == 0U ||
            media->frame_rate->denominator == 0U) {
            readiness.issues.push_back(
                PlatesetIssue{PlatesetIssueKind::NotVideo, slot});
            continue;
        }
        videos.emplace_back(slot, media);
    }
    if (readiness.filled == 0U) {
        readiness.issues.push_back(PlatesetIssue{PlatesetIssueKind::Empty, {}});
        return readiness;
    }
    if (plateset.slot(PlatesetSlot::Front).media == doc::NodeId{}) {
        readiness.issues.push_back(
            PlatesetIssue{PlatesetIssueKind::NoFront, PlatesetSlot::Front});
    }

    // The odd one out is judged against the front plate, which everything
    // syncs to, or without one against the rate most plates share.
    std::optional<MediaFrameRate> reference;
    std::size_t reference_votes = 0U;
    for (const auto& [slot, media] : videos) {
        if (slot == PlatesetSlot::Front) {
            reference = *media->frame_rate;
            break;
        }
        const auto votes = static_cast<std::size_t>(std::count_if(
            videos.begin(), videos.end(), [&media](const auto& other) {
                return same_rate(*other.second->frame_rate, *media->frame_rate);
            }));
        if (votes > reference_votes) {
            reference = *media->frame_rate;
            reference_votes = votes;
        }
    }
    bool shared_rate = reference.has_value();
    for (const auto& [slot, media] : videos) {
        if (!same_rate(*media->frame_rate, *reference)) {
            readiness.issues.push_back(
                PlatesetIssue{PlatesetIssueKind::FrameRateMismatch, slot});
            shared_rate = false;
        }
    }
    std::optional<bool> drop;
    for (const auto& [slot, media] : videos) {
        if (media->start_timecode.empty()) {
            continue;
        }
        const bool this_drop = timecode_is_drop_frame(media->start_timecode);
        if (!drop.has_value()) {
            drop = this_drop;
        } else if (*drop != this_drop) {
            readiness.issues.push_back(
                PlatesetIssue{PlatesetIssueKind::DropFrameMismatch, slot});
        }
    }
    if (shared_rate) {
        readiness.rate = *reference;
        core::i64 common = std::numeric_limits<core::i64>::max();
        for (const auto& [slot, media] : videos) {
            const core::i64 offset =
                std::max<core::i64>(plateset.slot(slot).offset, 0);
            common = std::min(
                common, static_cast<core::i64>(*media->frame_count) - offset);
        }
        if (common <= 0) {
            readiness.issues.push_back(
                PlatesetIssue{PlatesetIssueKind::NoCommonSpan, {}});
            common = 0;
        }
        readiness.common_frames = common;
    }

    for (const auto& [slot, media] : videos) {
        static_cast<void>(slot);
        core::u64 width = media->dimensions.has_value()
            ? media->dimensions->width
            : 0U;
        core::u64 height = media->dimensions.has_value()
            ? media->dimensions->height
            : 0U;
        if (media->proxy_playback_policy != ProxyPlaybackPolicy::Source &&
            media->requested_proxy_profile ==
                DerivativeProfileId::Proxy720pCfrH264 &&
            height > 720U) {
            width = (width * 720U + height / 2U) / height;
            height = 720U;
        }
        readiness.decode_pixels_per_second +=
            pixel_rate(width, height, *media->frame_rate);
    }
    if (readiness.decode_pixels_per_second > decode_budget) {
        readiness.issues.push_back(
            PlatesetIssue{PlatesetIssueKind::DecodeOverBudget, {}});
    }
    return readiness;
}

std::string plateset_issue_text(const PlatesetIssue& issue) {
    std::string slot = issue.slot.has_value()
        ? std::string{plateset_slot_label(*issue.slot)}
        : std::string{};
    switch (issue.kind) {
    case PlatesetIssueKind::Empty:
        return "No plates yet: drag media from the Library onto the slots";
    case PlatesetIssueKind::NoFront:
        return "FRONT is empty: the other plates sync against it";
    case PlatesetIssueKind::MissingMedia:
        return slot + ": its media is no longer in the Library";
    case PlatesetIssueKind::NotVideo:
        return slot + ": not a probed video, so it has no rate or length";
    case PlatesetIssueKind::FrameRateMismatch:
        return slot + ": runs at a different frame rate from the others";
    case PlatesetIssueKind::DropFrameMismatch:
        return slot + ": drop-frame timecode, unlike the others";
    case PlatesetIssueKind::NoCommonSpan:
        return "After the sync offsets no frame is covered by every plate";
    case PlatesetIssueKind::DecodeOverBudget:
        return "Decoding every plate exceeds the measured budget: prefer "
               "proxies";
    }
    return {};
}

PlatesetTimecodeResult
plateset_timecode_sync(const PlatesetAsset& plateset,
                       const AssetRegistry& registry) {
    PlatesetTimecodeResult result;
    std::vector<std::pair<PlatesetSlot, const MediaAsset*>> plates;
    for (const PlatesetSlot slot : all_plateset_slots) {
        const doc::NodeId id = plateset.slot(slot).media;
        if (id == doc::NodeId{}) {
            continue;
        }
        const Asset* const asset = registry.asset(id);
        const auto* const media =
            asset != nullptr ? std::get_if<MediaAsset>(asset) : nullptr;
        if (media == nullptr || media->start_timecode.empty() ||
            !media->frame_rate.has_value()) {
            result.slots.push_back(slot);
            continue;
        }
        plates.emplace_back(slot, media);
    }
    if (plates.empty() && result.slots.empty()) {
        result.error = PlatesetTimecodeError::Empty;
        return result;
    }
    if (!result.slots.empty()) {
        result.error = PlatesetTimecodeError::MissingTimecode;
        return result;
    }
    const MediaFrameRate rate = *plates.front().second->frame_rate;
    const bool drop = timecode_is_drop_frame(plates.front().second->start_timecode);
    for (const auto& [slot, media] : plates) {
        if (!same_rate(*media->frame_rate, rate)) {
            result.slots.push_back(slot);
        }
    }
    if (!result.slots.empty()) {
        result.error = PlatesetTimecodeError::FrameRateMismatch;
        return result;
    }
    for (const auto& [slot, media] : plates) {
        if (timecode_is_drop_frame(media->start_timecode) != drop) {
            result.slots.push_back(slot);
        }
    }
    if (!result.slots.empty()) {
        result.error = PlatesetTimecodeError::DropFrameMismatch;
        return result;
    }
    std::array<core::i64, plateset_slot_count> starts{};
    core::i64 latest = std::numeric_limits<core::i64>::min();
    for (const auto& [slot, media] : plates) {
        const std::optional<core::i64> start =
            timecode_frames(media->start_timecode, rate);
        if (!start.has_value()) {
            result.slots.push_back(slot);
            continue;
        }
        starts[static_cast<std::size_t>(slot)] = *start;
        latest = std::max(latest, *start);
    }
    if (!result.slots.empty()) {
        result.error = PlatesetTimecodeError::UnreadableTimecode;
        return result;
    }
    PlatesetTimecodeSync sync;
    for (const auto& [slot, media] : plates) {
        static_cast<void>(media);
        sync.offsets[static_cast<std::size_t>(slot)] =
            latest - starts[static_cast<std::size_t>(slot)];
    }
    result.sync = sync;
    return result;
}

std::string plateset_timecode_error_text(const PlatesetTimecodeResult& result) {
    std::string slots;
    for (const PlatesetSlot slot : result.slots) {
        if (!slots.empty()) {
            slots += ", ";
        }
        slots += plateset_slot_label(slot);
    }
    switch (result.error) {
    case PlatesetTimecodeError::Empty:
        return "No plates to sync";
    case PlatesetTimecodeError::MissingTimecode:
        return "No start timecode on " + slots +
            ": use Analyse Motion to sync these plates";
    case PlatesetTimecodeError::FrameRateMismatch:
        return "Not synced: " + slots + " run at a different frame rate";
    case PlatesetTimecodeError::DropFrameMismatch:
        return "Not synced: " + slots +
            " mix drop-frame and non-drop timecode";
    case PlatesetTimecodeError::UnreadableTimecode:
        return "Not synced: the timecode on " + slots +
            " does not read at its frame rate";
    }
    return {};
}

} // namespace mediaserver::assets
