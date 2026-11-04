#include "catalog_parts.hpp"

namespace ajfx::studio {

using namespace catalog_controls;

// Contact: what they hit.
void add_contact_blocks(std::vector<BlockDef>& blocks) {
    blocks.push_back(BlockDef{
        .kind = BlockKind::Ground,
        .title = "Ground",
        .description = "This layer's own ground.",
        .category = Category::Contact,
        .controls = {choice_control("response", "On contact", "bounce",
                                    {{"bounce", "Bounce"}, {"slide", "Slide"}, {"stick", "Stick"},
                                     // A body leaves the world when its life ends, never on contact.
                                     {"vanish", "Vanish", in_particles}},
                                    "At the ground and on solid things."),
                     when(publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2,
                                         "1 bounces back at full speed.")),
                          "response", {"bounce"}),
                     when(publishable(number_control("friction", "Friction", 0.2F, 0.0F, 1.0F, "", 2,
                                         "How fast sliding stops.", 0.0F, 1.0F)),
                          "response", {"bounce", "slide"}),
                     detail(number_control("height", "Height", 0.0F, -2.0F, 2.0F, "m", 2,
                                           "Ground height.", -1.0e6F))},
        .keywords = "floor collide collision plane bounce land",
    });
    // A particle layer meets the page's ground and every solid thing on the
    // page without being asked (see world_share). This is the way out.
    blocks.push_back(BlockDef{
        .kind = BlockKind::PassesThrough,
        .title = "Passes through",
        .description = "Goes through solid things.",
        .category = Category::Contact,
        .controls = {toggle_control("ground", "The ground too", false, "Falls through the page's ground.")},
        .keywords = "through ignore solid objects crates pieces bodies ground floor ghost no collision",
        .unique = true,
        // It takes modules away rather than adding any.
        .detachable = false,
        .layers = in_particles,
        .only_in = "Solid things always meet.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Sphere,
        .title = "Sphere collider",
        .description = "Keeps particles out of, or in, a ball.",
        .category = Category::Contact,
        .controls = {vec3_control("center", "Center", vec3(0.0F, 1.0F, 0.0F), "m", ""),
                     publishable(number_control("radius", "Radius", 0.5F, 0.0F, 5.0F, "m", 2, "")),
                     particles_only(toggle_control("inside", "Keep inside", false,
                                    "Keeps them inside.")),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     particles_only(detail(toggle_control("vanish", "Vanish on contact", false, "")))},
        .keywords = "collide collision ball obstacle",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Box,
        .title = "Box collider",
        .description = "Keeps particles out of, or in, a box.",
        .category = Category::Contact,
        .controls = {vec3_control("center", "Center", vec3(0.0F, 0.5F, 0.0F), "m", ""),
                     vec3_control("size", "Size", vec3(1.0F, 1.0F, 1.0F), "m", "Width, height, depth."),
                     angles_control("turn", "Turn", "How it is turned."),
                     particles_only(toggle_control("inside", "Keep inside", false,
                                    "Keeps them inside.")),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     particles_only(detail(toggle_control("vanish", "Vanish on contact", false, "")))},
        .keywords = "collide collision cube wall obstacle container",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::MeshCollider,
        .title = "Mesh collider",
        .description = "Particles bounce off your model.",
        .category = Category::Contact,
        .controls = {file_control("file", "Model", FileKind::Model,
                                  "OBJ, glTF or GLB. Baked to a distance field."),
                     vec3_control("position", "Position", vec3(0.0F, 0.0F, 0.0F), "m",
                                  "The model's origin."),
                     number_control("scale", "Scale", 1.0F, 0.1F, 4.0F, "x", 2,
                                    "Scales the model. Centimetres want 0.01.", 0.001F,
                                    1000.0F),
                     toggle_control("fit", "Fit", false,
                                    "Centres the model and makes its longest side the scale, as an object's model."),
                     angles_control("turn", "Turn", "How it is turned."),
                     choice_control("solid", "Treat as", "auto",
                                    {{"auto", "Automatic"}, {"solid", "Solid"}, {"surface", "Thin surface"}},
                                    "Solid, thin surface, or automatic."),
                     toggle_control("inside", "Keep inside", false, "Keeps them inside."),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     detail(choice_control("detail", "Detail", "medium",
                                           {{"coarse", "Coarse"}, {"medium", "Medium"}, {"fine", "Fine"}},
                                           "Finer follows the model closer, and exports bigger.")),
                     detail(toggle_control("vanish", "Vanish on contact", false, ""))},
        .keywords = "collide collision mesh model import obj gltf glb obstacle terrain sdf",
        // Its distance field is baked from the file; a Module could not name it.
        .detachable = false,
        .layers = in_particles,
        .only_in = "Use an Object.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Tumble,
        .title = "Tumble",
        .description = "Particles become spinning solid bodies.",
        .category = Category::Contact,
        .controls = {publishable(range_control("spin", "Spin", 180.0F, 720.0F, 0.0F, 2000.0F, "deg/s", 0,
                                   "Spin when thrown.")),
                     particles_only(publishable(number_control("roundness", "Roundness", 0.1F, 0.0F, 1.0F, "", 2,
                                                "0 slides flat, 1 rolls.",
                                                0.0F, 1.0F))),
                     particles_only(detail(publishable(number_control("damping", "Spin damping", 1.0F, 0.0F, 10.0F, "", 2,
                                           "How fast spin slows.")))),
                     particles_only(detail(publishable(number_control("settle", "Settle below", 0.2F, 0.0F, 2.0F, "m/s", 2,
                                           "Slower contacts stop bouncing.")))),
                     particles_only(detail(number_control("contact", "Contact size", 0.4F, 0.05F, 1.0F, "", 2,
                                           "Collision ball, as a share of size."))),
                     seed_control()},
        .keywords = "rigid body debris physics rotate rolling",
        .unique = true,
    });
}

} // namespace ajfx::studio
