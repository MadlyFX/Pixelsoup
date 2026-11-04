#pragma once

#include "document.hpp"
#include "scene.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The palette's starters: kinds of thing an artist can put on the stage
// that already answer when something happens to them, and the effects that
// make them answer, as templates to attach to any object.
// - A starter kind is an ordinary Placed Object (a kind with no body of its
//   own: every one of it is put on the stage or dropped while the scene
//   plays) with its model, size, material and break rule set, and its
//   effects attached. Nothing about it is special afterwards: the inspector
//   edits it as it edits any Object.
// - An attached effect is a particle system bound to the object by a rule
//   (a trail while it moves or smoulders, or a puff where it hits), styled
//   by a template. The system and the rule are ordinary scene items too,
//   and a template can be put back over them (apply_effect_template).
namespace ajfx::studio {

enum class StarterKind : std::uint8_t { SmoulderingLog, Crate, GlassPane, Brick, Rock, CardboardBox };

enum class EffectTemplate : std::uint8_t {
    SmokeTrail,  // grey smoke it gives off as it smoulders and moves
    EmberTrail,  // glowing embers it sheds, drifting up and falling
    DustOnHit,   // a puff of dust where it hits something
    SparksOnHit, // hot sparks where it hits hard
    GlintsOnHit, // bright splinters of light where it hits, as glass does
};

struct StarterKindInfo final {
    StarterKind kind{};
    std::string_view id;    // for scripts and the command line: "log", "crate"
    std::string_view name;  // what the artist sees: "Smouldering log"
    std::string_view about; // one line on what it does
};

struct EffectTemplateInfo final {
    EffectTemplate effect{};
    std::string_view id;   // "smoke", "embers", "dust", "sparks", "glints"
    std::string_view name; // "Smoke trail"
    RelationKind rule{RelationKind::TrailFrom};
    std::string_view about;
};

[[nodiscard]] std::span<const StarterKindInfo> starter_kinds();
[[nodiscard]] std::span<const EffectTemplateInfo> effect_templates();
[[nodiscard]] const StarterKindInfo &starter_kind_info(StarterKind kind);
[[nodiscard]] const EffectTemplateInfo &effect_template_info(EffectTemplate effect);
[[nodiscard]] std::optional<StarterKind> starter_kind(std::string_view id);
[[nodiscard]] std::optional<EffectTemplate> effect_template(std::string_view id);

// What a starter kind is made with: the effects it comes with, and the
// tint its dust takes (so a brick's dust is brick red).
[[nodiscard]] std::vector<EffectTemplate> starter_effects(StarterKind kind);

// Adds a starter kind standing at `position` with its effects attached;
// returns the Object, or 0 when the scene has no room for it (and then
// leaves the scene as it was).
[[nodiscard]] Uid add_starter_kind(Document &document, StarterKind kind, Vec3 position = {0.0F, 0.5F, 0.0F});

// Attaches a new particle system to `object`, bound by the template's rule
// and styled by it; `tint`, when given, colours it instead of the
// template's colour. Returns the system, named after the object and the
// effect ("Log smoke"); 0 when `object` is not an Object or there is no
// room.
[[nodiscard]] Uid attach_effect(Document &document, Uid object, EffectTemplate effect,
                                std::optional<Color> tint = std::nullopt);

// Puts a template's look and rule settings back over an attached effect:
// the system `system` and the rule that binds it. Its sources, name and
// what it is bound to are kept. False when `system` is bound by no rule of
// the template's kind (a trail template on a hit rule, say).
bool apply_effect_template(Document &document, Uid system, EffectTemplate effect,
                           std::optional<Color> tint = std::nullopt);

// Styles a system and its rule as `effect` does, without a document: what
// attach_effect and apply_effect_template both write.
void style_effect(ParticleSystem &system, Relationship &rule, EffectTemplate effect,
                  std::optional<Color> tint = std::nullopt);

// An effect attached to an object: the rule and the system it feeds.
struct AttachedEffect final {
    Uid rule{};
    Uid system{};
    RelationKind kind{RelationKind::TrailFrom};
};
// The effects bound to `object` by its own rules (not those on every
// object), in rule order.
[[nodiscard]] std::vector<AttachedEffect> attached_effects(const Scene &scene, Uid object);

} // namespace ajfx::studio
