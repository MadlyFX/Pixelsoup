#include "catalog_parts.hpp"

namespace ajfx::studio {

using namespace catalog_controls;

// Physics: solid pieces that collide with each other.
void add_rigid_blocks(std::vector<BlockDef>& blocks) {
    // One thing in the layer's world, and the blocks under it are its own.
    // It is the merge of what Pile and Prop were: an artist who wants a
    // crate to fracture places an Object and gives it a Break, rather than
    // reaching for a pile of one and finding that a second crate has to be
    // a prop, which cannot break at all.
    blocks.push_back(BlockDef{
        .kind = BlockKind::Object,
        .title = "Object",
        .description = "One thing in this layer.",
        .category = Category::Physics,
        .controls =
            {choice_control("model", "Model", "cube",
                            {{"layer", "The layer's own"},
                             {"cube", "Cube"},
                             {"rock", "Rock"},
                             {"slab", "Slab"},
                             {"shard", "Shard"},
                             {"file", "From a file"}},
                            "Shape of the object. The layer's own uses Appearance."),
             when(file_control("model_file", "Model file", FileKind::Model,
                               "OBJ, glTF or GLB, fitted to 1 m."),
                  "model", {"file"}),
             choice_control("arrange", "How many", "one",
                            {{"one", "Just one"}, {"heap", "A heap"}, {"stack", "A stack"}},
                            "Just one, a heap, or a stack."),
             when(number_control("count", "Pieces", 9.0F, 2.0F, 1000.0F, "", 0, "Pieces in the heap or stack.",
                                 2.0F, 20000.0F),
                  "arrange", {"heap", "stack"}),
             when(number_control("slope", "Slope", 55.0F, 15.0F, 80.0F, "deg", 0,
                                 "How steep the heap is.",
                                 5.0F, 85.0F),
                  "arrange", {"heap"}),
             when(number_control("width", "Width", 1.0F, 0.1F, 5.0F, "m", 2, "Stack width.",
                                 0.01F),
                  "arrange", {"stack"}),
             when(number_control("depth", "Depth", 0.4F, 0.1F, 5.0F, "m", 2,
                                 "Stack depth.", 0.01F),
                  "arrange", {"stack"}),
             number_control("size", "Size", 0.4F, 0.05F, 5.0F, "m", 2,
                            "The longest side.", 0.01F),
             // Picking one sets its weight, bounce and grip, and how its
             // Break comes apart, to that material's (blast.hpp). Each value
             // stays its own to change afterwards.
             choice_control("material", "Made of", "layer",
                            {{"layer", "The layer's"},
                             {"concrete", "Concrete"},
                             {"wood", "Wood"},
                             {"glass", "Glass"},
                             {"metal", "Metal"}},
                            "Sets its weight, bounce and how it breaks."),
             vec3_control("at", "Position", vec3(0.0F, 0.0F, 0.0F), "m",
                          "The middle of the object, or of its base."),
             angles_control("turn", "Turn", "How it is turned."),
             choice_control("motion", "It can", "loose",
                            {{"loose", "Be knocked about"}, {"fixed", "Stay put"}, {"driven", "Be moved"}},
                            "Knocked about, stay put, or moved."),
             choice_control("collides", "Collides as", "own",
                            {{"own", "Its own shape"},
                             {"ball", "A ball"},
                             {"block", "A block"},
                             {"bucket", "A bucket"},
                             {"crate", "A hollow crate"},
                             {"tube", "A tube"}},
                            "What other pieces hit. Bucket, crate and tube are hollow."),
             when(number_control("wall", "Wall thickness", 0.05F, 0.01F, 0.3F, "m", 2,
                                 "Wall thickness.", 0.001F),
                  "collides", {"bucket", "crate", "tube"}),
             when(number_control("radius", "Radius", 0.5F, 0.05F, 2.0F, "m", 2, "Width across.", 0.01F),
                  "collides", {"bucket", "tube", "ball"}),
             when(number_control("height", "Height", 0.8F, 0.05F, 3.0F, "m", 2, "Height.", 0.01F),
                  "collides", {"bucket", "tube"}),
             when(vec3_control("extent", "Size across", vec3(0.6F, 0.6F, 0.6F), "m",
                               "Width, height, depth."),
                  "collides", {"block"}),
             when(vec3_control("move_to", "Moves to", vec3(0.0F, 0.0F, 0.0F), "m",
                               "Where it ends up."),
                  "motion", {"driven"}),
             when(angles_control("turn_to", "Turns to",
                                 "The turn when done. Past 90, a bucket empties."),
                  "motion", {"driven"}),
             when(number_control("move_at", "Starts moving at", 0.5F, 0.0F, 10.0F, "s", 2, "When the move starts."),
                  "motion", {"driven"}),
             when(number_control("move_over", "Taking", 0.5F, 0.0F, 10.0F, "s", 2,
                                 "How long the move takes. 0 is instant."),
                  "motion", {"driven"}),
             number_control("release", "Let go at", 0.0F, 0.0F, 10.0F, "s", 2,
                            "When it is let go. 0 is at once."),
             // Born part way through, where it stands, and falling from
             // there: what the preview's Drop tool writes. Only one thing
             // the world moves can appear later; anything else stands from
             // the start.
             when(number_control("appear", "Appears at", 0.0F, 0.0F, 10.0F, "s", 2,
                                 "When it appears. 0 is from the start."),
                  "arrange", {"one"}),
             detail(toggle_control("own_material", "Made of its own thing", false,
                                   "Its own friction, bounce and density.")),
             detail(when(number_control("friction", "Friction", 0.6F, 0.0F, 1.0F, "", 2,
                                        "Grip.", 0.0F, 1.0F),
                         "own_material", {"on"})),
             detail(when(number_control("bounce", "Bounciness", 0.15F, 0.0F, 1.0F, "", 2,
                                        "1 bounces back at full speed.", 0.0F, 1.0F),
                         "own_material", {"on"})),
             detail(when(number_control("density", "Density", 2400.0F, 100.0F, 8000.0F, "kg/m^3", 0,
                                        "Wood is about 600, stone 2400.", 1.0F),
                         "own_material", {"on"})),
             detail(when(number_control("spacing", "Spacing", 1.0F, 0.5F, 2.0F, "x", 2,
                                        "How far apart they start. Below 1 is packed.",
                                        0.5F, 4.0F),
                         "arrange", {"heap", "stack"})),
             detail(when(number_control("jitter", "Untidiness", 0.15F, 0.0F, 1.0F, "", 2,
                                        "Nudge off its place.",
                                        0.0F, 1.0F),
                         "arrange", {"heap", "stack"})),
             detail(when(number_control("sides", "Sides", 12.0F, 3.0F, 48.0F, "", 0,
                                        "Flat sides of a round shape.",
                                        3.0F, 64.0F),
                         "collides", {"bucket", "tube"})),
             seed_control()},
        .keywords = "object thing crate box rock bucket wall barrel model import obj gltf glb place pile heap "
                    "stack prop scenery knock over tip spill fracture break piece material concrete wood glass "
                    "metal explode",
        // Its shape, pose and arrangement are the physics world's, not one
        // module's: the compiler makes a body kind or a prop of it.
        .detachable = false,
        .layers = in_rigid,
        .only_in = "Needs rigid physics.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Break,
        .title = "Break",
        .description = "Pieces come apart when hit hard.",
        .category = Category::Physics,
        .controls =
            {choice_control("into", "Comes apart into", "parts",
                            {{"parts", "Parts you place"}, {"cut", "Pieces cut from its model"}},
                            "Parts you place, or cut from its model."),
             when(number_control("cells", "Into", 8.0F, 2.0F, 64.0F, "", 0,
                                 "Pieces to cut into.",
                                 2.0F, 64.0F),
                  "into", {"cut"}),
             when(number_control("spread", "Cut towards the middle", 0.0F, 0.0F, 1.0F, "", 2,
                                 "0 cuts evenly, 1 crowds the middle.",
                                 0.0F, 1.0F),
                  "into", {"cut"}),
             when(detail(number_control("cut_seed", "Cut seed", 0.0F, 0.0F, 999.0F, "", 0,
                                        "Another cut.", 0.0F,
                                        1.0e6F)),
                  "into", {"cut"}),
             number_control("force", "Comes apart when hit at", 4.0F, 0.5F, 30.0F, "m/s", 1,
                            "Impact speed that breaks it.",
                            0.1F),
             // How hard an Explosion must throw it to shatter it where it
             // stands, rather than throw it whole. 0 leaves explosions to
             // push it.
             number_control("blast", "Breaks when blasted at", 0.0F, 0.0F, 40.0F, "m/s", 1,
                            "Blast speed that shatters it. 0: only pushed.", 0.0F),
             number_control("scatter", "Throwing the parts", 1.0F, 0.0F, 10.0F, "m/s", 2,
                            "Outward throw.", 0.0F),
             number_control("spin", "And tumbling them", 180.0F, 0.0F, 1440.0F, "deg/s", 0,
                            "Tumble on breaking.", 0.0F)},
        .keywords = "break shatter smash fracture crack apart destroy panels planks shards splinters debris blast "
                    "explode explosion",
        // One per Object rather than one per layer: a layer of several
        // things each comes apart in its own way, and the Object above a
        // Break is the one that breaks.
        .unique = false,
        // What it breaks into is the Part blocks under it.
        .detachable = false,
        .layers = in_rigid,
        .only_in = "Needs rigid physics.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Part,
        .title = "Part",
        .description = "One part a piece breaks into.",
        .category = Category::Physics,
        .controls =
            {choice_control("model", "Shape", "shard",
                            {{"shard", "Shard"},
                             {"slab", "Slab"},
                             {"rock", "Rock"},
                             {"cube", "Cube"},
                             {"file", "From a file"}},
                            "Shape of the part."),
             when(file_control("model_file", "Model", FileKind::Model,
                               "OBJ, glTF or GLB, fitted to 1 m."),
                  "model", {"file"}),
             number_control("size", "Size", 0.4F, 0.05F, 1.0F, "", 2,
                            "Part size, as a share of the piece.",
                            0.01F, 1.0F),
             vec3_control("at", "Sat at", vec3(0.0F, 0.5F, 0.0F), "",
                          "Where it sat, as a share of the piece's size."),
             angles_control("turn", "Turned", "How it is turned.")},
        .keywords = "part panel plank shard splinter chunk lid wall side piece fragment break into",
        .detachable = false,
        .layers = in_rigid,
        .only_in = "Needs rigid physics.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Physics,
        .title = "Bodies",
        .description = "What the pieces are made of.",
        .category = Category::Physics,
        .controls = {choice_control("shape", "Body", "auto",
                                    {{"auto", "Its own shape"}, {"ball", "Ball"}, {"cube", "Cube"}},
                                    "What each piece collides as."),
                     number_control("friction", "Friction", 0.6F, 0.0F, 1.0F, "", 2,
                                    "Grip.", 0.0F, 1.0F),
                     number_control("bounce", "Bounciness", 0.15F, 0.0F, 1.0F, "", 2,
                                    "1 bounces back at full speed.", 0.0F, 1.0F),
                     detail(number_control("density", "Density", 2400.0F, 100.0F, 8000.0F, "kg/m^3", 0,
                                           "Stone is about 2400.", 1.0F)),
                     detail(number_control("damping", "Spin damping", 0.05F, 0.0F, 2.0F, "", 2,
                                           "How fast spin slows.")),
                     detail(toggle_control("rest", "Rest when still", true,
                                           "Resting pieces sleep until knocked."))},
        .keywords = "rigid body physics collide stack pile topple jolt solid",
        .unique = true,
        // Part of the layer, so there is nothing to take it apart into.
        .detachable = false,
        // A rigid layer's world, held on the layer (Layer::world), not a
        // block an artist adds: there is one world per rigid layer and no
        // way to be without it. Never offered by the palette or the picker.
        .layers = in_no_layer,
    });
}

} // namespace ajfx::studio
