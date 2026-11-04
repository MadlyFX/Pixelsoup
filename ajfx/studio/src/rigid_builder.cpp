#include "rigid_builder.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cmath>

namespace ajfx::studio {
namespace {

// The size, in metres, of a particle nothing has sized: the runtime's own
// (simulation.cpp's default_size). A rigid layer with Objects and no Size block
// says it outright, so the Objects' scales are measured against a number the
// layer states and a Stream beside them is born at the size it had alone.
constexpr float unsized_particle = 0.1F;

} // namespace

RigidBuilder::RigidBuilder(BuildCore& core)
    : core_{core}, layer_{core.layer_}, s_{core.s_}, error_{core.error_}, physics_{core.physics_} {}

std::uint64_t RigidBuilder::extra_for_breaking() const noexcept {
    // Room the layer needs beyond the bodies it places, because a body that
    // comes apart is replaced by its pieces. The runtime refuses a break
    // that will not fit and counts what it refused, so sizing for it here is
    // what keeps a page from half-breaking without saying why.
    std::uint64_t extra = 0U;
    for (const ObjectBuild& of : objects_) {
        if (of.pieces > 1U) {
            extra += static_cast<std::uint64_t>(of.count) * (of.pieces - 1U);
        }
    }
    return extra;
}

// The rigid layer's own physics world (Layer::world), placed once from
// the layer rather than from a block on its list. A layer whose world
// is somehow missing takes the defaults, so a rigid layer always has
// one: that is what its kind promises.
void RigidBuilder::world() {
    if (!layer_.world) {
        place_after(SlotRigid, layer_.uid,
                    RigidPhysics{.id = module_id(layer_.uid, 4U), .density = 2400.0F, .friction = 0.6F,
                                 .restitution = 0.15F});
        return;
    }
    const Block& block = *layer_.world;
    const std::string shape = choice_of(block, "shape");
    place_after(SlotRigid, block.uid,
                RigidPhysics{.id = id(block, 1U),
                             .shape = shape == "ball"   ? RigidShape::Sphere
                                      : shape == "cube" ? RigidShape::Box
                                                        : RigidShape::Auto,
                             .density = number_of(block, "density"),
                             .friction = number_of(block, "friction"),
                             .restitution = number_of(block, "bounce"),
                             .angular_damping = number_of(block, "damping"),
                             .allow_sleep = toggle_of(block, "rest")});
}

// The model a block's choice control names, as a package entry: a
// library mesh by name, the artist's own file, or empty for the layer's.
std::string RigidBuilder::model_of(const Block& block, std::string_view control, std::string_view file_control,
                                   std::string_view fallback) {
    std::string name = choice_of(block, control);
    if (name == "layer") {
        return {};
    }
    if (name == "file") {
        const std::string path = file_of(block, file_control);
        // No model picked yet: the fallback, until there is one.
        return path.empty() ? std::string{fallback} : request(AssetRequest::Kind::Mesh, block, path);
    }
    return name;
}

// The material a body kind takes: the Object's own where it says so,
// else the layer's, since a part of a crate is made of what the crate is.
void RigidBuilder::material_of(RigidBodyKind& of, const Block& block, bool own) {
    const Block* from = own ? &block : (layer_.world ? &*layer_.world : nullptr);
    if (from == nullptr) {
        return;
    }
    of.density = number_of(*from, "density");
    of.friction = number_of(*from, "friction");
    of.restitution = number_of(*from, own ? "bounce" : "bounce");
}

// The body kind a Stream's or Burst's newborns are. 0 is the layer's own,
// which Appearance draws and the Bodies row makes; a model of its own is a
// kind of its own, drawn and hit as that model and made of what the
// layer's Bodies row says, like the parts a piece breaks into. Its size is
// still the layer's, so scale stays 1. A particle layer has no bodies and
// never stores a model, so this is always 0 there.
std::uint32_t RigidBuilder::born_as(const Block& block) {
    if (!physics_) {
        return 0U;
    }
    const std::string model = model_of(block, "model", "model_file", "cube");
    if (model.empty()) {
        return 0U;
    }
    RigidBodyKind of;
    of.shape = RigidShape::Hull;
    of.mesh = model;
    of.draw = model;
    material_of(of, block, false);
    kinds_.push_back(std::move(of));
    return static_cast<std::uint32_t>(kinds_.size());
}

// One Object of the layer: a thing standing in its world, which is a
// body kind of its own where it can be and a prop where it cannot. It
// opens a group, so the breaking blocks after it are its.
void RigidBuilder::object(const Block& block) {
    finish_object();
    ObjectBuild out;
    out.block = block.uid;
    out.title = block_summary(block);
    const std::string arrange = choice_of(block, "arrange");
    const std::string motion = choice_of(block, "motion");
    const std::string collides = choice_of(block, "collides");
    const bool hollow = collides == "bucket" || collides == "crate" || collides == "tube";
    const bool own_material = toggle_of(block, "own_material");
    // A kind placed by hand has no body of its own: they are all put on the
    // stage or dropped.
    const bool placed = arrange == "placed";
    out.count = placed             ? 0U
                : arrange == "one" ? 1U
                                   : static_cast<std::uint32_t>(std::max(1L, std::lround(number_of(block, "count"))));
    out.model = model_of(block, "model", "model_file", "cube");
    const std::string& model = out.model;
    const float size = std::max(number_of(block, "size") * s_, 1.0e-4F);

    // A body is moved by the world and collides as a sphere, a cube or
    // its model's hull. Anything else an artist may ask for -- standing
    // still, being driven, or being hollow so things sit inside it -- is
    // a prop, which is the only thing the engine can make of it.
    if (motion == "loose" && !hollow) {
        RigidBodyKind of;
        // Its own shape means its model's hull. An Object drawn as the
        // layer's own pieces has no model to wrap, so it takes the
        // layer's shape instead, which is what every other body is:
        // Auto is a hull where the layer draws models and a ball where
        // it draws sprites, so a heap of sprites still has bodies.
        of.shape = collides == "ball"    ? RigidShape::Sphere
                   : collides == "block" ? RigidShape::Box
                   : model.empty()       ? RigidShape::Auto
                                         : RigidShape::Hull;
        if (of.shape == RigidShape::Hull) {
            of.mesh = model;
        }
        of.draw = model;
        material_of(of, block, own_material);
        kinds_.push_back(std::move(of));
        out.kind = static_cast<std::uint32_t>(kinds_.size());
        // Its size is the kind's, so one layer holds a crate and the
        // girder beside it. The scale is worked out in build(), once the
        // size every body is born at is known.
        object_sizes_.emplace_back(out.kind, size);
        // One thing that appears part way through ("Appears at", as the
        // preview's Drop tool writes it) is born then, where it stands, and
        // falls from there: a burst of one of its kind, placed and held
        // still by init modules that act on its kind alone. A piece it
        // breaks into is not born, so these never move one.
        const float appear = arrange == "one" ? std::max(number_of(block, "appear"), 0.0F) : 0.0F;
        if (appear > 0.0F) {
            const ModuleId burst = id(block, 1U);
            place_spawn(block.uid, SpawnBurst{.id = burst, .time = appear, .count = 1U, .body = out.kind});
            appearing_.push_back(burst);
            const Scope only{.sources = {}, .kinds = {out.kind}};
            core_.place_init(SlotRawInit, block.uid,
                             ShapeLocation{.id = id(block, 2U),
                                           .shape = Shape::Sphere,
                                           .center = Vec3Input{to_vec3(get(block, "at")) * s_},
                                           .radius_min = FloatInput{0.0F},
                                           .radius_max = FloatInput{0.0F},
                                           .only = only});
            core_.place_init(SlotRawInit, block.uid,
                             Velocity{.id = id(block, 3U),
                                      .speed_min = FloatInput{0.0F},
                                      .speed_max = FloatInput{0.0F},
                                      .only = only});
            objects_.push_back(std::move(out));
            open_ = objects_.size() - 1U;
            return;
        }
        place_spawn(block.uid,
                    SpawnPile{.id = id(block, 1U),
                              .count = out.count,
                              .shape = arrange == "one"     ? PileShape::Single
                                       : arrange == "stack" ? PileShape::Box
                                                            : PileShape::Heap,
                              .center = to_vec3(get(block, "at")) * s_,
                              .slope = number_of(block, "slope"),
                              .width = number_of(block, "width") * s_,
                              .depth = number_of(block, "depth") * s_,
                              .spacing = number_of(block, "spacing"),
                              .jitter = arrange == "one" ? 0.0F : number_of(block, "jitter"),
                              .release = number_of(block, "release"),
                              .body = out.kind});
    } else {
        out.prop = true;
        if (placed) {
            error_ = "A thing placed by hand has to be knocked about: let " + std::string{block_title(block)} +
                     " move, or give it a place of its own";
            return;
        }
        if (out.count > 1U) {
            error_ = "Only one " + std::string{block_title(block)} +
                     " can stand still, be moved or be hollow: set how many back to just one, or let it be "
                     "knocked about";
            return;
        }
        if (motion == "driven" && collides == "own") {
            error_ = "An object that is moved cannot collide as its own shape: have it collide as a hollow "
                     "crate, a bucket or a tube instead";
            return;
        }
        prop(block, model, size, motion, collides, hollow);
    }
    objects_.push_back(std::move(out));
    open_ = objects_.size() - 1U;
}

// When the Object above comes apart. What it breaks into is the Part
// blocks under it, so the module is placed once they are all in; a block
// set to cut its model names a cut and places nothing, since the model is
// not read until the builder reads it.
void RigidBuilder::breaking(const Block& block) {
    // With no Object above it there is nothing to break. The layer
    // plays on unbroken and the coach asks for one, as a cut that
    // cannot be made does: half an instruction is not a refusal.
    if (open_ == no_object) {
        return;
    }
    ObjectBuild& of = objects_[open_];
    if (of.breaking || of.cut) {
        error_ = "An object can come apart only one way, so it has one Break";
        return;
    }
    Break made{.id = id(block, 1U),
               .body = of.kind,
               .threshold = number_of(block, "force") * s_,
               .blast_threshold = std::max(number_of(block, "blast"), 0.0F) * s_,
               .scatter = number_of(block, "scatter") * s_,
               .spin = number_of(block, "spin")};
    if (choice_of(block, "into") == "cut") {
        of.pieces = static_cast<std::uint32_t>(std::max(1L, std::lround(number_of(block, "cells"))));
        of.cut = FractureCut{.layer = layer_.uid,
                             .block = block.uid,
                             .body = of.kind,
                             .id = made.id,
                             .cells = of.pieces,
                             .seed = static_cast<std::uint32_t>(std::lround(number_of(block, "cut_seed"))),
                             .spread = number_of(block, "spread"),
                             .threshold = made.threshold,
                             .blast_threshold = made.blast_threshold,
                             .scatter = made.scatter,
                             .spin = made.spin};
        return;
    }
    of.breaking = std::move(made);
}

// One part the Object above comes apart into. Each is a body kind of its
// own, collided and drawn as that part's model.
void RigidBuilder::part(const Block& block) {
    if (open_ == no_object) {
        return;
    }
    RigidBodyKind of;
    of.shape = RigidShape::Hull;
    of.mesh = model_of(block, "model", "model_file", "shard");
    if (of.mesh.empty()) {
        of.mesh = "shard";
    }
    of.draw = of.mesh;
    material_of(of, block, false);
    kinds_.push_back(std::move(of));
    ObjectBuild& owner = objects_[open_];
    owner.parts.push_back(BreakPiece{.body = static_cast<std::uint32_t>(kinds_.size()),
                                     .offset = to_vec3(get(block, "at")),
                                     .turn = to_vec3(get(block, "turn")),
                                     .size = number_of(block, "size")});
    owner.pieces = static_cast<std::uint32_t>(owner.parts.size());
}

// Places what the Object that is open came apart into, and closes it.
// Breaking is two halves and either alone does nothing, so a Break with
// no parts beside it is left out rather than guessed at; the coach says
// which half is missing.
void RigidBuilder::finish_object() {
    if (open_ == no_object) {
        return;
    }
    ObjectBuild& of = objects_[open_];
    if (of.prop && (of.breaking || of.cut)) {
        error_ = "Only a piece the world moves can come apart: let this object be knocked about, and have "
                 "it collide as its own shape, a ball or a block";
        open_ = no_object;
        return;
    }
    if (of.breaking && !of.parts.empty()) {
        of.breaking->pieces = of.parts;
        place_after(SlotBreak, of.block, *of.breaking);
    }
    if (of.cut) {
        // The model cut is the Object's own, so a layer of several cuts
        // each one's own rather than whatever the layer draws.
        of.cut->model = of.model;
        cuts_.push_back(*of.cut);
    }
    open_ = no_object;
}

// An Object the engine cannot make a body of: one that stands still, one
// the effect drives, or one that is hollow so pieces sit inside it. It
// becomes a prop, which is authored rather than born. Props are
// collected here and attached to the RigidPhysics module in build(),
// since that module may be placed by a block that comes after this one.
void RigidBuilder::prop(const Block& block, const std::string& model, float size, const std::string& motion,
                        const std::string& collides, bool hollow) {
    RigidProp out;
    out.motion = motion == "driven" ? PropMotion::Driven
                 : motion == "loose" ? PropMotion::Loose
                                     : PropMotion::Fixed;
    if (collides == "bucket") {
        out.shape = PropShape::HollowCylinder;
    } else if (collides == "crate") {
        out.shape = PropShape::HollowBox;
    } else if (collides == "tube") {
        out.shape = PropShape::Pipe;
    } else if (collides == "ball") {
        out.shape = PropShape::Sphere;
    } else if (collides == "block") {
        out.shape = PropShape::Box;
    } else {
        out.shape = PropShape::Mesh;
    }
    static_cast<void>(hollow);
    // The model is what a host draws, whatever the prop collides as, so
    // it is named whenever the Object has one.
    if (!model.empty()) {
        out.mesh = model;
    } else if (out.shape == PropShape::Mesh) {
        // Nothing to follow: stand in as a block of its size, so the
        // layer still plays while the coach asks for a model.
        out.shape = PropShape::Box;
    }
    out.position = to_vec3(get(block, "at")) * s_;
    out.turn = to_vec3(get(block, "turn"));
    out.size = size;
    out.half_size = to_vec3(get(block, "extent")) * (0.5F * s_);
    out.radius = number_of(block, "radius") * s_;
    out.height = number_of(block, "height") * s_;
    out.wall = number_of(block, "wall") * s_;
    out.segments = static_cast<std::uint32_t>(std::lround(number_of(block, "sides")));
    out.density = number_of(block, "density");
    out.friction = number_of(block, "friction");
    out.restitution = number_of(block, "bounce");
    // A move the artist has not touched leaves the prop where it stands,
    // rather than carrying it to the origin.
    const Value* to_position = block.find("move_to");
    const Value* to_turn = block.find("turn_to");
    out.to_position = to_position != nullptr ? to_vec3(*to_position) * s_ : out.position;
    out.to_turn = to_turn != nullptr ? to_vec3(*to_turn) : out.turn;
    out.move_from = number_of(block, "move_at");
    out.move_over = number_of(block, "move_over");
    props_.push_back(std::move(out));
    prop_blocks_.push_back(block.uid);
}

// Once every block is in: each Object's size against the size every body is born
// at, whether the layer's kinds of body fit a world, and the props and bodies
// attached to the physics module any block may have placed.
bool RigidBuilder::finish(LayerReport& report) {
    // An Object's size is its own, carried by its body kind's scale. It
    // is a multiple of the size every body is born at. With no Size block
    // that is the size a particle has when nothing sizes it, stated here
    // rather than left to the runtime, so the Objects' scales are
    // measured against it and a Stream in the layer is no bigger for
    // having an Object beside it.
    if (!object_sizes_.empty()) {
        if (core_.size_min_ <= 0.0F) {
            core_.size_min_ = unsized_particle;
            core_.place_init(SlotSize, layer_.uid,
                       InitSize{.id = module_id(layer_.uid, 5U), .min = unsized_particle, .max = unsized_particle});
        }
        for (const auto& [kind, size] : object_sizes_) {
            if (kind >= 1U && kind <= kinds_.size()) {
                kinds_[kind - 1U].scale = size / core_.size_min_;
            }
        }
    }
    // Every Object is a kind, every Part is a kind and every cell a cut
    // makes is a kind, so a page of several cutting Objects can ask for
    // more than a world may hold. It is refused here, naming what wanted
    // them, rather than at the far end of the builder.
    std::size_t wanted = kinds_.size();
    for (const FractureCut& cut : cuts_) {
        wanted += cut.cells;
    }
    if (wanted > max_rigid_body_kinds) {
        report.error = "This layer's objects need " + std::to_string(wanted) + " kinds of body, and a world " +
                       "holds " + std::to_string(max_rigid_body_kinds) + ": cut fewer pieces or place fewer " +
                       "parts";
        report.error_block = objects_.empty() ? layer_.uid : objects_.back().block;
        return false;
    }
    report.props = prop_blocks_;
    report.appearing = appearing_;
    // The props and the parts' body kinds go on the layer's RigidPhysics
    // module, which any block may have placed, so they are attached once
    // every block is in. A part is drawn as its own model only where the
    // layer draws models at all; otherwise it still hits things as its
    // own shape and is drawn as the layer's pieces are.
    if (!props_.empty() || !kinds_.empty()) {
        // The layer's renderer, which reaches the emitter further down.
        const bool draws_models = core_.renderer_.geometry == Geometry::Mesh;
        for (Placed& placed : core_.after_) {
            auto* update = std::get_if<UpdateModule>(&placed.line.module);
            auto* physics = update != nullptr ? std::get_if<RigidPhysics>(update) : nullptr;
            if (physics != nullptr) {
                physics->props = std::move(props_);
                if (!draws_models) {
                    for (RigidBodyKind& of : kinds_) {
                        of.draw.clear();
                    }
                }
                physics->bodies = std::move(kinds_);
                break;
            }
        }
    }
    return true;
}

} // namespace ajfx::studio
