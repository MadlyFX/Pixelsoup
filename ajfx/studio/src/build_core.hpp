#pragma once

#include "catalog.hpp"
#include "compile.hpp"
#include "document.hpp"

#include <ajfx/effect.hpp>
#include <ajfx/modules.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// What the compiler's builders share (layer_builder.cpp, rigid_builder.cpp):
// where a module goes within its stage, the ids a block's modules are given,
// and the state one layer's build accumulates. Internal to the compiler;
// nothing outside it includes this.
namespace ajfx::studio {

// Module ids: sixteen a uid, so each block (and each layer, for what the
// compiler makes from it) owns its own run of ids.
constexpr ModuleId module_id(Uid uid, std::uint32_t slot) noexcept {
    return uid * 16U + slot;
}

// Where a module goes within its stage. Lower first; ties keep page order.
enum Slot : int {
    // init
    SlotLifetime = 0,
    SlotShape,
    SlotVelocity,
    SlotColor,
    SlotSize,
    SlotRotation,
    SlotSpin,
    SlotTumble,
    SlotRawInit,
    // update: forces
    SlotGravity = 0,
    SlotRise,
    SlotWind,
    SlotSwirl,
    SlotAttract,
    SlotDrawIn,
    SlotTurbulence,
    SlotShockwave,
    SlotWake,
    SlotRawForce,
    SlotDrag, // last: it slows what the forces gave
    // update: after integration
    SlotRigid = 0,
    SlotBreak, // after the world it breaks bodies in
    SlotCollision,
    SlotRawCollision,
};

struct Placed final {
    int slot{};
    ModuleLine line;
};

[[nodiscard]] inline Vec3 to_vec3(const Value& value) {
    return Vec3{value.x, value.y, value.z};
}

class RigidBuilder;

// The state a layer's build accumulates and the services every block's
// compile step draws on: the layer and the page's scale, the module lists a
// block places into, the ids its modules are given, the artist's files it
// asks for and the first thing that went wrong. The layer builder compiles
// the blocks every layer has and the rigid builder those only a physics
// layer has; both work on this.
class BuildCore {
public:
    // `world` is the page's ground, gravity and wind, which every layer gets
    // by its share (world_share). `solids` is the one world the page's solid
    // layers play in (SolidWorld), which a particle layer meets unless it
    // passes through and which blocks born of solid things read; empty where
    // the page has none, or while the solid layers themselves build.
    // `blasts` are the page's explosions (page_blasts), which every layer
    // answers unless it passes through.
    BuildCore(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters, PageWorld world,
              SolidWorld solids, std::vector<PageBlast> blasts = {});

    // The parameters this layer's blocks publish.
    [[nodiscard]] const std::vector<std::string>& published() const noexcept { return published_; }
    // What a baked layer held rather than published.
    [[nodiscard]] const std::vector<LayerReport::Held>& held() const noexcept { return held_; }
    // The artist's files the layer's blocks name.
    [[nodiscard]] const std::vector<AssetRequest>& requests() const noexcept { return requests_; }

protected:
    // A block's module id. Random streams are keyed by id, so a block's seed
    // picks another id to deal its particles other values. Seeded ids have
    // the top bit set, apart from every plain uid * 16 + slot, and the rare
    // clash of two seeded ones moves on to the next free id.
    ModuleId id(const Block& block, std::uint32_t slot);
    // The id of the module that meets the page's solid world: keyed by the
    // layer's uid and `solid` (0 for the whole world), so it is the same
    // wherever the layer sits on the page, and kept apart from every other
    // id the layer gives, as a seeded id is.
    ModuleId meeting_id(Uid solid);
    // The id of the module that answers the Explosion block `blast`: keyed
    // by the layer's uid and the block's, as a meeting is, so it stays put
    // wherever either sits.
    ModuleId blast_id(Uid blast);
    // The id of the Wake the page's solid world stirs the layer with when
    // no block says otherwise: keyed by the layer's uid alone.
    ModuleId wake_id();

    // Names the package entry an artist's file goes in, and asks for it.
    std::string request(AssetRequest::Kind kind, const Block& block, const std::string& path, float scale = 1.0F,
                        std::uint32_t cells = 0U, ColliderSolid solid = ColliderSolid::Auto, bool fit = false,
                        Vec3 turn = {});

    template <class M>
    void place_init(int slot, Uid origin, M module) {
        init_.push_back(Placed{slot, ModuleLine{origin, RawModule{InitModule{std::move(module)}}}});
    }
    template <class M>
    void place_force(int slot, Uid origin, M module) {
        forces_.push_back(Placed{slot, ModuleLine{origin, RawModule{UpdateModule{std::move(module)}}}});
    }
    template <class M>
    void place_after(int slot, Uid origin, M module) {
        after_.push_back(Placed{slot, ModuleLine{origin, RawModule{UpdateModule{std::move(module)}}}});
    }
    template <class M>
    void place_spawn(Uid origin, M module) {
        spawn_.push_back(Placed{0, ModuleLine{origin, RawModule{SpawnModule{std::move(module)}}}});
    }

    const Layer& layer_;
    float s_;
    std::vector<ParameterDesc>& parameters_;
    std::vector<std::string> published_;
    std::vector<LayerReport::Held> held_;
    std::vector<AssetRequest> requests_;
    std::string error_;
    std::set<std::string> unique_;
    std::set<ModuleId> seeded_;
    bool physics_{false};
    bool saw_appearance_{false};
    PageWorld world_;
    SolidWorld solids_;
    std::vector<PageBlast> blasts_;
    // The size every body is born at, which an Object's own size is a
    // multiple of. 0 until a Size block says.
    float size_min_{0.0F};
    std::vector<Placed> spawn_;
    std::vector<Placed> init_;
    std::vector<Placed> forces_;
    std::vector<Placed> after_;
    std::vector<Placed> life_;
    std::vector<std::function<Color(float)>> color_life_;
    std::vector<std::function<float(float)>> size_life_;
    std::vector<Uid> life_origins_;
    std::optional<ModuleLine> raw_color_life_;
    std::optional<ModuleLine> raw_size_life_;
    SpriteRenderer renderer_{};

    friend class RigidBuilder;
};

} // namespace ajfx::studio
