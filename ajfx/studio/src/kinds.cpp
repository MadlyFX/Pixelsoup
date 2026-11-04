#include "kinds.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace ajfx::studio {
namespace {

constexpr std::array<StarterKindInfo, 6> kinds{{
    {StarterKind::SmoulderingLog, "log", "Smouldering log",
     "A log that smokes and sheds embers, more so as it tumbles"},
    {StarterKind::Crate, "crate", "Crate", "A light wooden crate that breaks into eight when hit hard, with dust"},
    {StarterKind::GlassPane, "glass", "Glass pane", "A pane that shatters at a knock into twelve, with glints"},
    {StarterKind::Brick, "brick", "Brick", "A heavy brick that cracks in four on a hard hit, with red dust"},
    {StarterKind::Rock, "rock", "Rock", "A heavy rock that never breaks; it strikes sparks and dust"},
    {StarterKind::CardboardBox, "box", "Cardboard box", "An empty box, light enough for the wind to carry off"},
}};

constexpr std::array<EffectTemplateInfo, 5> templates{{
    {EffectTemplate::SmokeTrail, "smoke", "Smoke trail", RelationKind::TrailFrom,
     "Grey smoke it gives off all the time, and more while it moves"},
    {EffectTemplate::EmberTrail, "embers", "Embers", RelationKind::TrailFrom,
     "Glowing embers it sheds, drifting up and falling"},
    {EffectTemplate::DustOnHit, "dust", "Dust on hit", RelationKind::EmitOnHit,
     "A puff of dust where it hits something, bigger for harder hits"},
    {EffectTemplate::SparksOnHit, "sparks", "Sparks on hit", RelationKind::EmitOnHit,
     "Hot sparks where it hits hard; soft knocks make none"},
    {EffectTemplate::GlintsOnHit, "glints", "Glints on hit", RelationKind::EmitOnHit,
     "Bright splinters of light where it hits, as glass does"},
}};

// What a template calls the system it makes, after the object's name.
std::string_view suffix(EffectTemplate effect) {
    switch (effect) {
    case EffectTemplate::SmokeTrail:
        return " smoke";
    case EffectTemplate::EmberTrail:
        return " embers";
    case EffectTemplate::DustOnHit:
        return " dust";
    case EffectTemplate::SparksOnHit:
        return " sparks";
    case EffectTemplate::GlintsOnHit:
        return " glints";
    }
    return " effect";
}

// Fades from `alpha` to nothing over its life, keeping its tint (the keys
// multiply it).
std::vector<ColorKey> fade(float alpha) {
    return {{0.0F, Color{1.0F, 1.0F, 1.0F, alpha}}, {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}};
}

// The Motion stack a template's particles move by, as fresh behaviors.
bool add_motion(Document &document, Uid system, EffectTemplate effect) {
    SceneItem *item = document.scene->item(system);
    auto &particles = std::get<ParticleSystem>(item->data);
    particles.motion.clear();
    const auto add = [&](BehaviorKind kind, Range strength, float size = 0.6F, float churn = 0.4F) {
        const Uid uid = add_behavior(document, system, kind);
        if (uid == 0U) {
            return false;
        }
        auto &stack = std::get<ParticleSystem>(document.scene->item(system)->data).motion;
        MotionBehavior &behavior = stack.back();
        behavior.strength = strength;
        behavior.size = size;
        behavior.churn = churn;
        return true;
    };
    switch (effect) {
    case EffectTemplate::SmokeTrail:
        return add(BehaviorKind::Turbulence, {0.4F, 0.8F}, 0.8F, 0.3F) && add(BehaviorKind::Drag, {0.6F, 0.9F});
    case EffectTemplate::EmberTrail:
        return add(BehaviorKind::Turbulence, {0.6F, 1.2F}, 0.4F, 0.6F) && add(BehaviorKind::Drag, {0.4F, 0.7F});
    case EffectTemplate::DustOnHit:
        return add(BehaviorKind::Drag, {2.0F, 3.0F}) && add(BehaviorKind::Turbulence, {0.3F, 0.5F}, 0.5F, 0.3F);
    case EffectTemplate::SparksOnHit:
        return add(BehaviorKind::Drag, {0.2F, 0.4F});
    case EffectTemplate::GlintsOnHit:
        return add(BehaviorKind::Drag, {0.5F, 1.0F});
    }
    return true;
}

Relationship *rule_of(Scene &scene, Uid system, RelationKind kind) {
    for (Relationship &rule : scene.relationships) {
        if (rule.system == system && rule.kind == kind) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace

std::span<const StarterKindInfo> starter_kinds() {
    return kinds;
}

std::span<const EffectTemplateInfo> effect_templates() {
    return templates;
}

const StarterKindInfo &starter_kind_info(StarterKind kind) {
    return kinds[static_cast<std::size_t>(kind)];
}

const EffectTemplateInfo &effect_template_info(EffectTemplate effect) {
    return templates[static_cast<std::size_t>(effect)];
}

std::optional<StarterKind> starter_kind(std::string_view id) {
    for (const StarterKindInfo &info : kinds) {
        if (info.id == id) {
            return info.kind;
        }
    }
    return std::nullopt;
}

std::optional<EffectTemplate> effect_template(std::string_view id) {
    for (const EffectTemplateInfo &info : templates) {
        if (info.id == id) {
            return info.effect;
        }
    }
    return std::nullopt;
}

std::vector<EffectTemplate> starter_effects(StarterKind kind) {
    switch (kind) {
    case StarterKind::SmoulderingLog:
        return {EffectTemplate::SmokeTrail, EffectTemplate::EmberTrail};
    case StarterKind::Crate:
    case StarterKind::Brick:
        return {EffectTemplate::DustOnHit};
    case StarterKind::GlassPane:
        return {EffectTemplate::GlintsOnHit};
    case StarterKind::Rock:
        return {EffectTemplate::SparksOnHit, EffectTemplate::DustOnHit};
    case StarterKind::CardboardBox:
        return {};
    }
    return {};
}

void style_effect(ParticleSystem &p, Relationship &rule, EffectTemplate effect, std::optional<Color> tint) {
    p.radius = {0.0F, 0.05F};
    p.persistent = false;
    p.collide = true;
    p.spin = {0.0F, 0.0F};
    p.lighting = ParticleLighting::Glows;
    p.casts_light = false;
    p.size_driver = Driver::Constant;
    p.size_over_age = {{0.0F, 1.0F}, {1.0F, 1.0F}};
    p.tint_driver = Driver::Age;
    rule.seed = 0U;
    switch (effect) {
    case EffectTemplate::SmokeTrail: {
        const Color color = tint.value_or(Color{0.42F, 0.4F, 0.38F, 1.0F});
        p.look = ParticleLook::Smoke;
        p.lighting = ParticleLighting::Lit;
        p.tint = color;
        p.brightness = {0.8F, 1.0F};
        p.lifetime = {2.0F, 3.5F};
        p.direction = {0.0F, 1.0F, 0.0F};
        p.speed = {0.15F, 0.35F};
        p.spread = 25.0F;
        p.gravity = 0.0F; // warm: it keeps rising at its own speed
        p.size = {0.12F, 0.22F};
        p.size_driver = Driver::Age;
        p.size_over_age = {{0.0F, 0.6F}, {1.0F, 3.0F}};
        p.tint_over_age = fade(0.7F);
        p.collide = false;
        // Smoulders at rest and smokes harder as it tumbles.
        rule.rate = 5.0F;
        rule.per_speed = 6.0F;
        rule.min_speed = 0.0F;
        rule.pieces = false;
        break;
    }
    case EffectTemplate::EmberTrail: {
        const Color color = tint.value_or(Color{1.0F, 0.45F, 0.12F, 1.0F});
        p.look = ParticleLook::Glow;
        p.tint = color;
        p.brightness = {2.0F, 4.0F};
        p.lifetime = {0.8F, 1.6F};
        p.direction = {0.0F, 1.0F, 0.0F};
        p.speed = {0.4F, 1.2F};
        p.spread = 50.0F;
        p.gravity = 0.15F;
        p.size = {0.012F, 0.025F};
        p.tint_over_age = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{0.6F, 0.1F, 0.02F, 0.0F}}};
        rule.rate = 3.0F;
        rule.per_speed = 8.0F;
        rule.min_speed = 0.0F;
        rule.pieces = false;
        break;
    }
    case EffectTemplate::DustOnHit: {
        const Color color = tint.value_or(Color{0.62F, 0.56F, 0.47F, 1.0F});
        p.look = ParticleLook::Smoke;
        p.lighting = ParticleLighting::Lit;
        p.tint = color;
        p.brightness = {0.8F, 1.0F};
        p.lifetime = {0.8F, 1.8F};
        p.direction = {0.0F, 1.0F, 0.0F};
        p.speed = {0.3F, 1.2F};
        p.spread = 70.0F;
        p.gravity = 0.05F;
        p.size = {0.06F, 0.14F};
        p.size_driver = Driver::Age;
        p.size_over_age = {{0.0F, 0.7F}, {1.0F, 2.5F}};
        p.tint_over_age = fade(0.8F);
        rule.hardness = 1.0F;
        rule.per_hit = 6.0F;
        rule.per_speed = 4.0F;
        rule.spread = 80.0F;
        break;
    }
    case EffectTemplate::SparksOnHit: {
        const Color color = tint.value_or(Color{1.0F, 0.68F, 0.3F, 1.0F});
        p.look = ParticleLook::Glow;
        p.tint = color;
        p.brightness = {3.0F, 6.0F};
        p.lifetime = {0.3F, 0.8F};
        p.direction = {0.0F, 1.0F, 0.0F};
        p.speed = {2.0F, 5.0F};
        p.spread = 60.0F;
        p.gravity = 1.0F;
        p.size = {0.008F, 0.018F};
        p.tint_over_age = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{1.0F, 0.25F, 0.05F, 0.0F}}};
        // Only hard knocks strike sparks.
        rule.hardness = 3.0F;
        rule.per_hit = 8.0F;
        rule.per_speed = 4.0F;
        rule.spread = 60.0F;
        break;
    }
    case EffectTemplate::GlintsOnHit: {
        const Color color = tint.value_or(Color{0.8F, 0.92F, 1.0F, 1.0F});
        p.look = ParticleLook::Glow;
        p.tint = color;
        p.brightness = {1.5F, 3.0F};
        p.lifetime = {0.4F, 1.0F};
        p.direction = {0.0F, 1.0F, 0.0F};
        p.speed = {1.0F, 3.0F};
        p.spread = 75.0F;
        p.gravity = 1.0F;
        p.size = {0.006F, 0.014F};
        p.tint_over_age = fade(1.0F);
        rule.hardness = 1.5F;
        rule.per_hit = 10.0F;
        rule.per_speed = 3.0F;
        rule.spread = 75.0F;
        break;
    }
    }
}

Uid attach_effect(Document &document, Uid object, EffectTemplate effect, std::optional<Color> tint) {
    if (!document.scene) {
        return 0U;
    }
    const SceneItem *owner = document.scene->item(object);
    if (owner == nullptr || !std::holds_alternative<Object>(owner->data)) {
        return 0U;
    }
    const std::string name = owner->name + std::string{suffix(effect)};
    const RelationKind kind = effect_template_info(effect).rule;
    const Uid system = add_bound_system(document, kind, object);
    if (system == 0U) {
        return 0U;
    }
    document.scene->item(system)->name = name;
    if (!apply_effect_template(document, system, effect, tint)) {
        if (Relationship *rule = rule_of(*document.scene, system, kind)) {
            static_cast<void>(remove_relationship(document, rule->uid));
        }
        static_cast<void>(remove_item(document, system));
        return 0U;
    }
    return system;
}

bool apply_effect_template(Document &document, Uid system, EffectTemplate effect, std::optional<Color> tint) {
    if (!document.scene) {
        return false;
    }
    SceneItem *item = document.scene->item(system);
    if (item == nullptr || !std::holds_alternative<ParticleSystem>(item->data)) {
        return false;
    }
    Relationship *rule = rule_of(*document.scene, system, effect_template_info(effect).rule);
    if (rule == nullptr) {
        return false;
    }
    style_effect(std::get<ParticleSystem>(item->data), *rule, effect, tint);
    return add_motion(document, system, effect);
}

Uid add_starter_kind(Document &document, StarterKind kind, Vec3 position) {
    if (!document.scene) {
        return 0U;
    }
    const Document before = document;
    const char *mesh = kind == StarterKind::Rock ? "rock"
                       : kind == StarterKind::Crate || kind == StarterKind::Brick || kind == StarterKind::CardboardBox
                           ? "cube"
                           : "slab";
    const Uid uid = add_object(document, mesh);
    if (uid == 0U) {
        return 0U;
    }
    SceneItem *item = document.scene->item(uid);
    item->name = std::string{starter_kind_info(kind).name};
    auto &o = std::get<Object>(item->data);
    o.distribution = Distribution::Placed;
    o.motion = ObjectMotion::Dynamic;
    o.collider = ObjectCollider::Hull;
    o.position = position;
    std::optional<Color> dust;
    switch (kind) {
    case StarterKind::SmoulderingLog:
        o.size = 0.6F;
        o.tint = {0.42F, 0.3F, 0.2F, 1.0F};
        o.density = 600.0F;
        o.friction = 0.7F;
        o.restitution = 0.1F;
        break;
    case StarterKind::Crate:
        o.size = 0.5F;
        o.tint = {0.66F, 0.5F, 0.32F, 1.0F};
        o.collider = ObjectCollider::Box;
        o.density = 300.0F; // a box of thin boards, mostly air
        o.friction = 0.6F;
        o.restitution = 0.15F;
        o.fracture = true;
        o.fragments = 8U;
        o.fracture_speed = 5.0F;
        break;
    case StarterKind::GlassPane:
        o.size = 0.8F;
        o.tint = {0.72F, 0.86F, 0.95F, 1.0F};
        o.density = 2500.0F;
        o.friction = 0.3F;
        o.restitution = 0.1F;
        o.fracture = true;
        o.fragments = 12U;
        o.fracture_speed = 2.5F;
        break;
    case StarterKind::Brick:
        o.size = 0.25F;
        o.tint = {0.62F, 0.3F, 0.22F, 1.0F};
        o.collider = ObjectCollider::Box;
        o.density = 1900.0F;
        o.friction = 0.8F;
        o.restitution = 0.05F;
        o.fracture = true;
        o.fragments = 4U;
        o.fracture_speed = 7.0F;
        dust = Color{0.7F, 0.42F, 0.32F, 1.0F};
        break;
    case StarterKind::Rock:
        o.size = 0.4F;
        o.tint = {0.5F, 0.5F, 0.48F, 1.0F};
        o.density = 2600.0F;
        o.friction = 0.8F;
        o.restitution = 0.2F;
        dust = Color{0.55F, 0.54F, 0.52F, 1.0F};
        break;
    case StarterKind::CardboardBox:
        o.size = 0.4F;
        o.tint = {0.74F, 0.6F, 0.42F, 1.0F};
        o.collider = ObjectCollider::Box;
        o.density = 25.0F; // thin card round a box of air
        o.friction = 0.5F;
        o.restitution = 0.2F;
        break;
    }
    for (const EffectTemplate effect : starter_effects(kind)) {
        const bool dusty = effect == EffectTemplate::DustOnHit;
        if (attach_effect(document, uid, effect, dusty ? dust : std::nullopt) == 0U) {
            document = before;
            return 0U;
        }
    }
    return uid;
}

std::vector<AttachedEffect> attached_effects(const Scene &scene, Uid object) {
    std::vector<AttachedEffect> out;
    if (object == 0U) {
        return out;
    }
    for (const Relationship &rule : scene.relationships) {
        if (rule.object == object && (rule.kind == RelationKind::EmitOnHit || rule.kind == RelationKind::TrailFrom)) {
            out.push_back(AttachedEffect{.rule = rule.uid, .system = rule.system, .kind = rule.kind});
        }
    }
    return out;
}

} // namespace ajfx::studio
