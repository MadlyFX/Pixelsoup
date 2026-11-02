#pragma once

#include "build_core.hpp"

#include <ajfx/modules.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The part of a layer's build that only a rigid physics layer has: its world,
// the Objects standing in it, what they come apart into, and the props and
// bodies that end up on the world's module. A rigid layer is one physics
// world, so these are not forces or looks that merge into curves; each
// Object is a body kind (or a prop) of its own, and the blocks under it
// belong to it.
namespace ajfx::studio {

class RigidBuilder final {
public:
    explicit RigidBuilder(BuildCore& core);

    // The layer's own physics world (Layer::world), placed once from the
    // layer rather than from a block on its list.
    void world();
    // The body kind a Stream's or Burst's newborns are; 0 is the layer's own.
    std::uint32_t born_as(const Block& block);
    // One Object: a body kind where the world can move it, a prop where it
    // cannot. It opens a group, so the breaking blocks after it are its.
    void object(const Block& block);
    // When the Object above comes apart, and what into.
    void breaking(const Block& block);
    void part(const Block& block);
    // Places what the Object that is open came apart into, and closes it.
    void finish_object();
    // Once every block is in. False, with the report saying why, when the
    // layer's Objects need more kinds of body than a world holds.
    bool finish(LayerReport& report);

    // The cuts this layer's Break blocks asked for, in page order. Each
    // names the body kind that comes apart and the model to cut, so a layer
    // of several Objects cuts each one's own model.
    [[nodiscard]] const std::vector<FractureCut>& cuts() const noexcept { return cuts_; }
    // Room the layer needs beyond the bodies it places.
    [[nodiscard]] std::uint64_t extra_for_breaking() const noexcept;

private:
    std::string model_of(const Block& block, std::string_view control, std::string_view file_control,
                         std::string_view fallback);
    void material_of(RigidBodyKind& of, const Block& block, bool own);
    void prop(const Block& block, const std::string& model, float size, const std::string& motion,
              const std::string& collides, bool hollow);

    // The core's services, under the names the block code reads them by.
    ModuleId id(const Block& block, std::uint32_t slot) { return core_.id(block, slot); }
    std::string request(AssetRequest::Kind kind, const Block& block, const std::string& path) {
        return core_.request(kind, block, path);
    }
    template <class M>
    void place_after(int slot, Uid origin, M module) {
        core_.place_after(slot, origin, std::move(module));
    }
    template <class M>
    void place_spawn(Uid origin, M module) {
        core_.place_spawn(origin, std::move(module));
    }

    // One Object of the layer, and what it comes apart into. An Object owns
    // the breaking blocks that follow it, so `open_` is the one they reach.
    struct ObjectBuild final {
        Uid block{};
        std::string title;
        // The model it draws and collides as, empty for the layer's own.
        std::string model;
        // The body kind it is, or 0 when it stands as a prop instead.
        std::uint32_t kind{0U};
        bool prop{false};
        std::optional<Break> breaking;
        std::vector<BreakPiece> parts;
        std::optional<FractureCut> cut;
        // How many bodies it places, and what one of them comes apart into,
        // for the layer's budget.
        std::uint32_t count{1U};
        std::uint32_t pieces{1U};
    };

    BuildCore& core_;
    const Layer& layer_;
    float s_;
    std::string& error_;
    const bool& physics_;
    // The props the layer's Objects describe, in page order.
    std::vector<RigidProp> props_;
    // The Object each prop is, in the same order (LayerReport::props).
    std::vector<Uid> prop_blocks_;
    // The bursts of Objects that appear part way through, by module id
    // (LayerReport::appearing).
    std::vector<ModuleId> appearing_;
    // The body kinds the layer's Objects and their Parts make, in the order
    // the blocks are listed. Kind 0 is the emitter's own, so the kind a push
    // makes is kinds_.size() once it is in.
    std::vector<RigidBodyKind> kinds_;
    std::vector<ObjectBuild> objects_;
    static constexpr std::size_t no_object = static_cast<std::size_t>(-1);
    std::size_t open_{no_object};
    std::vector<FractureCut> cuts_;
    // Each body-kind Object's kind and the size it asked for, turned into
    // the kind's scale in finish() once the size every body is born at is
    // known.
    std::vector<std::pair<std::uint32_t, float>> object_sizes_;
};

} // namespace ajfx::studio
