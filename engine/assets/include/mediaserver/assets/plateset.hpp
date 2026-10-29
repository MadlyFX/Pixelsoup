#pragma once

#include <mediaserver/assets/asset_registry.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Vehicle process platesets: the slot vocabulary, timecode arithmetic, slot
// proposals from file names, and the readiness a plateset has to pass before
// it is placed. Everything here is a pure function of authored asset data.
namespace mediaserver::assets {

// The document key of a slot (`front_right`), its operator label, which is
// the LED Car Process socket it wires to (`FRONT RIGHT 3/4`), and a short tag
// for tight spaces (`FR`).
[[nodiscard]] std::string_view plateset_slot_key(PlatesetSlot slot) noexcept;
[[nodiscard]] std::string_view plateset_slot_label(PlatesetSlot slot) noexcept;
[[nodiscard]] std::string_view plateset_slot_tag(PlatesetSlot slot) noexcept;
[[nodiscard]] std::string plateset_slot_property(PlatesetSlot slot);
[[nodiscard]] std::string plateset_offset_property(PlatesetSlot slot);
// `moment_<index>_<field>`, field being frame, name or color.
[[nodiscard]] std::string plateset_moment_property(std::size_t index,
                                                   std::string_view field);

// The slots a rig template offers, in port order.
[[nodiscard]] std::span<const PlatesetSlot>
plateset_rig_slots(PlatesetRig rig) noexcept;
[[nodiscard]] std::string_view plateset_rig_key(PlatesetRig rig) noexcept;
[[nodiscard]] std::optional<PlatesetRig>
plateset_rig_from_key(std::string_view key) noexcept;
[[nodiscard]] std::string_view plateset_rig_label(PlatesetRig rig) noexcept;

inline constexpr std::array<PlatesetSlot, plateset_slot_count>
    all_plateset_slots{
        PlatesetSlot::Front,    PlatesetSlot::FrontRight, PlatesetSlot::Right,
        PlatesetSlot::BackRight, PlatesetSlot::Back,      PlatesetSlot::BackLeft,
        PlatesetSlot::Left,     PlatesetSlot::FrontLeft,  PlatesetSlot::Ceiling,
    };

// SMPTE timecode at `rate`. `timecode_frames` reads "HH:MM:SS:FF" (a ';' or
// '.' before the frames marks drop-frame) as a frame number, or nothing when
// it is not a timecode at that rate; `timecode_text` writes one back.
// Drop-frame applies only at 29.97 and 59.94 and their multiples; any other
// rate counts every frame.
[[nodiscard]] std::optional<core::i64>
timecode_frames(std::string_view timecode, MediaFrameRate rate);
[[nodiscard]] std::string timecode_text(core::i64 frame, MediaFrameRate rate,
                                        bool drop_frame);
[[nodiscard]] bool timecode_is_drop_frame(std::string_view timecode) noexcept;

// The slot a camera file's name says it is, from the tags camera arrays use
// (`FC`, `FRONT`, `BL`, `REAR_LEFT`, `TOP`...), or nothing when the name
// says no position. The last tag in the name wins, because arrays put the
// position after the take (`DP230703-042-...-FL_1.mp4`).
[[nodiscard]] std::optional<PlatesetSlot>
propose_plateset_slot(std::string_view name) noexcept;

struct PlatesetProposal final {
    std::array<doc::NodeId, plateset_slot_count> slots{};
    // Media whose name says no slot, or a slot another file already took.
    std::vector<doc::NodeId> unplaced;
    // The smallest rig template that offers every proposed slot.
    PlatesetRig rig{PlatesetRig::Nine};

    bool operator==(const PlatesetProposal&) const = default;
};

// Slot assignments for `media`, in the order given; the first file to claim
// a slot keeps it. Proposals are never authored on their own: the operator
// sees them in the slots and moves any that are wrong.
[[nodiscard]] PlatesetProposal
propose_plateset(std::span<const MediaAsset* const> media);

// What placing a plateset would decode, per second, against what this
// installation was measured to keep up with: four 3840x2160 24 fps plates
// on the reference Director (RTX 3090) held a 0.975 ready ratio, with no
// headroom left. Proxy-preferring slots count at their proxy's size.
inline constexpr core::u64 plateset_decode_budget_pixels_per_second =
    4ULL * 3'840ULL * 2'160ULL * 24ULL;

enum class PlatesetIssueKind : core::u8 {
    // No slot holds anything.
    Empty,
    // Every rig drives from the front plate; without it there is nothing to
    // sync the others against.
    NoFront,
    // The slot names a media asset that is no longer in the Library.
    MissingMedia,
    // The slot's media is a still or an unprobed file: it has no rate or
    // length to sync.
    NotVideo,
    // Plates run at different rates; syncing would have to drop or repeat
    // frames, so it is refused rather than rounded.
    FrameRateMismatch,
    // Some plates carry drop-frame timecode and others do not.
    DropFrameMismatch,
    // After the sync offsets there is no frame every plate covers.
    NoCommonSpan,
    // Decoding every plate would exceed the measured budget.
    DecodeOverBudget,
};

struct PlatesetIssue final {
    PlatesetIssueKind kind{PlatesetIssueKind::Empty};
    // The slot the issue is about, when it is about one.
    std::optional<PlatesetSlot> slot;

    // Blocking issues refuse placement; the decode budget is a warning,
    // because proxies or Half resolution can still bring it in.
    [[nodiscard]] bool blocking() const noexcept {
        return kind != PlatesetIssueKind::DecodeOverBudget;
    }

    bool operator==(const PlatesetIssue&) const = default;
};

struct PlatesetReadiness final {
    std::vector<PlatesetIssue> issues;
    core::u32 filled{};
    // The plates' shared rate, once they share one.
    std::optional<MediaFrameRate> rate;
    // Source frames every plate covers after its offset; zero when there is
    // no such span or no shared rate.
    core::i64 common_frames{};
    core::u64 decode_pixels_per_second{};
    core::u64 decode_budget{plateset_decode_budget_pixels_per_second};

    [[nodiscard]] bool ready() const noexcept;

    bool operator==(const PlatesetReadiness&) const = default;
};

[[nodiscard]] PlatesetReadiness evaluate_plateset(
    const PlatesetAsset& plateset, const AssetRegistry& registry,
    core::u64 decode_budget = plateset_decode_budget_pixels_per_second);

// One line an operator can act on, naming the slot when there is one.
[[nodiscard]] std::string plateset_issue_text(const PlatesetIssue& issue);

// Offsets that line the plates up by their start timecode: the plate that
// starts last gets zero and every other plate skips the frames it rolled
// before it. Nothing when a filled slot has no timecode, the plates do not
// share a rate, or they mix drop-frame and non-drop timecode.
struct PlatesetTimecodeSync final {
    std::array<core::i64, plateset_slot_count> offsets{};
    // Slots without timecode, which make the sync impossible.
    std::vector<PlatesetSlot> missing;

    bool operator==(const PlatesetTimecodeSync&) const = default;
};

enum class PlatesetTimecodeError : core::u8 {
    Empty,
    MissingTimecode,
    FrameRateMismatch,
    DropFrameMismatch,
    UnreadableTimecode,
};

struct PlatesetTimecodeResult final {
    std::optional<PlatesetTimecodeSync> sync;
    PlatesetTimecodeError error{PlatesetTimecodeError::Empty};
    // The slots the error is about.
    std::vector<PlatesetSlot> slots;

    bool operator==(const PlatesetTimecodeResult&) const = default;
};

[[nodiscard]] PlatesetTimecodeResult
plateset_timecode_sync(const PlatesetAsset& plateset,
                       const AssetRegistry& registry);
[[nodiscard]] std::string
plateset_timecode_error_text(const PlatesetTimecodeResult& result);

} // namespace mediaserver::assets
