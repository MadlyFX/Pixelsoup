#include "test_support.hpp"

#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/core/frame_arena.hpp>
#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/core/strong_id.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using FirstId = mediaserver::core::StrongId<struct FirstTag, std::uint64_t>;
using SecondId = mediaserver::core::StrongId<struct SecondTag, std::uint64_t>;

static_assert(!std::is_convertible_v<FirstId, SecondId>);
static_assert(!std::is_convertible_v<std::uint64_t, FirstId>);

MS_TEST("strong IDs preserve type boundaries") {
    MS_CHECK(FirstId{7U}.value() == 7U);
    MS_CHECK(FirstId{7U} == FirstId{7U});
    MS_CHECK(FirstId{7U} != FirstId{8U});
}

MS_TEST("partial asset activation retries and converges on a common commit") {
    using namespace mediaserver::core;
    AssetPublicationGate preview, output;
    AssetSetReadiness local{19, true, 0}, remote{19, true, 0};
    const auto cohort = [&] {
        AssetCohort result;
        result.observe(&local);
        result.observe(&remote);
        return result;
    };
    const AssetCommit first{19, 10, 42};
    MS_CHECK(cohort().needs_commit({}, 1));
    MS_CHECK(preview.present(local, first, 10, 42, true));
    local.visible_commit = preview.visible_commit();
    // A ready output that misses the exact frame cannot reveal late, even
    // while the show is parked on the original show frame.
    MS_CHECK(!output.present(remote, first, 11, 42, true));
    MS_CHECK(cohort().some_visible && !cohort().visible);
    MS_CHECK(!cohort().needs_commit(first, 9));
    MS_CHECK(!cohort().needs_commit(first, 10));
    const auto retry_due =
        first.control_frame + asset_commit_confirmation_frames;
    MS_CHECK(!cohort().needs_commit(first, retry_due - 1));
    MS_CHECK(cohort().needs_commit(first, retry_due));

    const AssetCommit retry{19, retry_due + 1 + asset_commit_lead_frames, 42};
    // Catch-up must offer the fresh activation to the preview too.
    MS_CHECK(asset_presentation_frame(local, retry, retry_due) ==
             retry.control_frame);
    MS_CHECK(asset_presentation_frame(remote, retry, retry_due) ==
             retry.control_frame);
    MS_CHECK(asset_presentation_frame(local, retry, retry.control_frame) ==
             retry.control_frame);
    MS_CHECK(asset_presentation_frame(local, retry, retry.control_frame + 1) ==
             0);
    MS_CHECK(!cohort().needs_commit(retry, retry_due));
    MS_CHECK(!output.present(remote, retry, retry.control_frame - 1, 42, true));
    MS_CHECK(!output.present(remote, retry, retry.control_frame, 43, true));
    // A visible member keeps displaying, but must not confirm early, late,
    // or with the wrong show frame.
    MS_CHECK(preview.present(local, retry, retry.control_frame - 1, 42, true));
    MS_CHECK(preview.visible_commit() == first.control_frame);
    MS_CHECK(preview.present(local, retry, retry.control_frame, 43, true));
    MS_CHECK(preview.visible_commit() == first.control_frame);
    MS_CHECK(output.present(remote, retry, retry.control_frame, 42, true));
    remote.visible_commit = output.visible_commit();
    MS_CHECK(preview.present(local, retry, retry.control_frame + 1, 42, true));
    MS_CHECK(preview.visible_commit() == first.control_frame);

    // Both now show the same assets but confirmed different attempts. Another
    // retry is still necessary, and must converge without reloading assets.
    MS_CHECK(!cohort().visible && cohort().can_commit());
    MS_CHECK(cohort().needs_commit(
        retry, retry.control_frame + asset_commit_confirmation_frames));
    const AssetCommit final{19,
                            retry.control_frame +
                                asset_commit_confirmation_frames + 1 +
                                asset_commit_lead_frames,
                            100};
    MS_CHECK(asset_presentation_frame(local, final, retry.control_frame + 1) ==
             final.control_frame);
    MS_CHECK(asset_presentation_frame(remote, final, retry.control_frame + 1) ==
             final.control_frame);
    MS_CHECK(preview.present(local, final, final.control_frame,
                             final.show_frame, true));
    MS_CHECK(output.present(remote, final, final.control_frame,
                            final.show_frame, true));
    local.visible_commit = preview.visible_commit();
    remote.visible_commit = output.visible_commit();
    MS_CHECK(local.visible_commit == final.control_frame);
    MS_CHECK(remote.visible_commit == final.control_frame);
    MS_CHECK(cohort().visible);
    MS_CHECK(!cohort().needs_commit(final, final.control_frame + 1000));
    MS_CHECK(asset_presentation_frame(local, final, final.control_frame) == 0);
    MS_CHECK(asset_presentation_frame(local, retry, retry.control_frame) == 0);
    // An obsolete permit cannot regress the acknowledgement.
    MS_CHECK(preview.present(local, first, first.control_frame,
                             first.show_frame, true));
    MS_CHECK(preview.visible_commit() == final.control_frame);
}

MS_TEST("asset retries require every member ready for the same generation") {
    using namespace mediaserver::core;
    const AssetCommit previous{19, 10, 42};
    const AssetSetReadiness visible{19, true, 10};
    const std::array blocked_members{
        AssetSetReadiness{19, false, 0},
        AssetSetReadiness{29, true, 10},
        AssetSetReadiness{0, true, 0},
    };
    MS_CHECK(!AssetCohort{}.needs_commit({}, 100));
    AssetCohort missing;
    missing.observe(&visible);
    missing.observe(nullptr);
    MS_CHECK(!missing.needs_commit(previous, 100));
    for (const auto &blocked : blocked_members) {
        AssetCohort cohort;
        cohort.observe(&visible);
        cohort.observe(&blocked);
        MS_CHECK(!cohort.visible);
        MS_CHECK(!cohort.needs_commit(previous, 100));
    }
    AssetCohort replacement;
    const AssetSetReadiness next{29, true, 0};
    replacement.observe(&next);
    replacement.observe(&next);
    MS_CHECK(replacement.needs_commit(previous, 11));
    MS_CHECK(!asset_commit_confirmation_expired(0, 100));
    MS_CHECK(!asset_commit_confirmation_expired(
        std::numeric_limits<u64>::max() - 10, std::numeric_limits<u64>::max()));
}

MS_TEST(
    "a missed output joins an already visible preview on the next activation") {
    using namespace mediaserver::core;
    AssetPublicationGate preview, output;
    AssetSetReadiness local{19, true, 0}, remote{19, true, 0};
    const AssetCommit first{19, 10, 42};
    MS_CHECK(preview.present(local, first, 10, 42, true));
    local.visible_commit = preview.visible_commit();
    MS_CHECK(!output.present(remote, first, 11, 43, true));
    AssetCohort partial;
    partial.observe(&local);
    partial.observe(&remote);
    const auto retry_due =
        first.control_frame + asset_commit_confirmation_frames;
    MS_CHECK(partial.needs_commit(first, retry_due));
    const AssetCommit retry{19, retry_due + 1 + asset_commit_lead_frames, 103};
    // Neither an unreleased permit nor a different generation can advance
    // the preview's acknowledgement of an otherwise visible asset set.
    MS_CHECK(preview.present(local, {}, retry.control_frame, retry.show_frame,
                             true));
    MS_CHECK(preview.present(local, {29, retry.control_frame, retry.show_frame},
                             retry.control_frame, retry.show_frame, true));
    MS_CHECK(preview.visible_commit() == first.control_frame);
    MS_CHECK(preview.present(local, retry, retry.control_frame,
                             retry.show_frame, true));
    MS_CHECK(output.present(remote, retry, retry.control_frame,
                            retry.show_frame, true));
    local.visible_commit = preview.visible_commit();
    remote.visible_commit = output.visible_commit();
    AssetCohort recovered;
    recovered.observe(&local);
    recovered.observe(&remote);
    MS_CHECK(recovered.visible &&
             recovered.visible_commit == retry.control_frame);
    MS_CHECK(!recovered.needs_commit(retry, retry.control_frame + 1000));
}

MS_TEST("frame arena aligns, exhausts, and resets caller-owned memory") {
    alignas(64) std::array<std::byte, 128U> storage{};
    mediaserver::core::FrameArena arena{storage};

    const auto first = arena.allocate(7U, 1U);
    const auto aligned = arena.allocate(16U, 16U);
    MS_CHECK(first != nullptr);
    MS_CHECK(aligned != nullptr);
    MS_CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 16U == 0U);
    MS_CHECK(arena.used() >= 23U);
    MS_CHECK(arena.allocate(256U) == nullptr);

    arena.reset();
    MS_CHECK(arena.used() == 0U);
    MS_CHECK(arena.remaining() == arena.capacity());
}


// Honest idle reporting: a feature drawing nothing must say why, and must say
// it once. Repeating a reason every frame would be worse than the silence this
// replaces, so the dedup is the contract rather than a nicety.
MS_TEST("an idle reason is reported once and then stays quiet") {
    static const char feature[] = "unit test feature";
    static const char first[] = "the first distinct reason";
    static const char second[] = "a different reason";

    MS_CHECK(mediaserver::core::report_idle_once(feature, first));
    // Same reason, same pointer: already said.
    MS_CHECK(!mediaserver::core::report_idle_once(feature, first));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, first));
    // A different reason is still heard - one noisy condition must not mask
    // another, which is the whole point of naming them separately.
    MS_CHECK(mediaserver::core::report_idle_once(feature, second));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, second));

    // Nothing to say, and nothing that crashes saying it.
    MS_CHECK(!mediaserver::core::report_idle_once(nullptr, first));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, nullptr));
}
} // namespace
