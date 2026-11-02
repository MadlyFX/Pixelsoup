#include "document_io.hpp"

#include "catalog.hpp"
#include "json.hpp"
#include "raw_modules.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace ajfx::studio {
namespace {

json::Value value_json(const ControlDef& control, const Value& value) {
    json::Value array = json::Value::array();
    const auto push = [&](float v) { array.push(json::Value::of(static_cast<double>(v))); };
    switch (control.type) {
    case ControlType::Number: return json::Value::of(static_cast<double>(value.x));
    case ControlType::Toggle: return json::Value::of(value.x != 0.0F);
    case ControlType::Choice:
    case ControlType::File: return json::Value::of(value.text);
    case ControlType::Range:
        push(value.x);
        push(value.y);
        return array;
    case ControlType::Vec3:
        push(value.x);
        push(value.y);
        push(value.z);
        return array;
    case ControlType::Color:
        push(value.x);
        push(value.y);
        push(value.z);
        push(value.w);
        return array;
    }
    return json::Value{};
}

std::optional<Value> value_from(const ControlDef& control, const json::Value& in) {
    Value value;
    const auto floats = [&](std::size_t count) {
        if (in.type != json::Value::Type::Array || in.items.size() != count) {
            return false;
        }
        float* out[4] = {&value.x, &value.y, &value.z, &value.w};
        for (std::size_t i = 0U; i < count; ++i) {
            if (in.items[i].type != json::Value::Type::Number) {
                return false;
            }
            *out[i] = static_cast<float>(in.items[i].number);
        }
        return true;
    };
    bool ok = false;
    switch (control.type) {
    case ControlType::Number:
        ok = in.type == json::Value::Type::Number;
        value.x = static_cast<float>(in.number);
        break;
    case ControlType::Toggle:
        ok = in.type == json::Value::Type::Bool;
        value.x = in.boolean ? 1.0F : 0.0F;
        break;
    case ControlType::Choice:
    case ControlType::File:
        ok = in.type == json::Value::Type::String;
        value.text = in.text;
        break;
    case ControlType::Range:
        // A lone number is a page saved while the control was one number.
        if (in.type == json::Value::Type::Number) {
            ok = true;
            value.y = static_cast<float>(in.number);
            value.x = control.number_is_high ? 0.0F : value.y;
        } else {
            ok = floats(2U);
        }
        break;
    case ControlType::Vec3: ok = floats(3U); break;
    case ControlType::Color: ok = floats(4U); break;
    }
    if (!ok) {
        return std::nullopt;
    }
    return sanitize(control, std::move(value));
}

std::string_view backdrop_name(Backdrop backdrop) {
    switch (backdrop) {
    case Backdrop::Studio: return "studio";
    case Backdrop::Night: return "night";
    case Backdrop::Void: return "void";
    }
    return "night";
}

Backdrop backdrop_from(std::string_view name) {
    if (name == "studio") {
        return Backdrop::Studio;
    }
    return name == "void" ? Backdrop::Void : Backdrop::Night;
}

} // namespace

json::Value block_to_json(const Block& block) {
    json::Value b = json::Value::object();
    b.add("uid", json::Value::of(static_cast<double>(block.uid)));
    b.add("kind", json::Value::of(block.kind));
    if (!block.enabled) {
        b.add("enabled", json::Value::of(false));
    }
    if (block.raw) {
        b.add("module", raw_to_json(*block.raw));
    }
    const BlockDef* def = find_block_def(block.kind);
    json::Value values = json::Value::object();
    if (def != nullptr) {
        // In the catalog's order, so a saved page reads like the block.
        for (const ControlDef& control : def->controls) {
            if (const Value* stored = block.find(control.id); stored != nullptr) {
                values.add(std::string{control.id}, value_json(control, *stored));
            }
        }
    }
    if (!values.members.empty()) {
        b.add("values", std::move(values));
    }
    if (!block.published.empty()) {
        json::Value published = json::Value::array();
        for (const Published& entry : block.published) {
            json::Value p = json::Value::object();
            p.add("control", json::Value::of(entry.control));
            p.add("name", json::Value::of(entry.name));
            json::Value range = json::Value::array();
            range.push(json::Value::of(static_cast<double>(entry.low)));
            range.push(json::Value::of(static_cast<double>(entry.high)));
            p.add("range", std::move(range));
            published.push(std::move(p));
        }
        b.add("published", std::move(published));
    }
    if (block.detached) {
        json::Value d = json::Value::object();
        d.add("from", json::Value::of(static_cast<double>(block.detached->from)));
        json::ParseError ignored;
        if (std::optional<json::Value> original = json::parse(block.detached->original, ignored)) {
            d.add("block", std::move(*original));
        }
        d.add("module", raw_to_json(block.detached->module));
        d.add("count", json::Value::of(static_cast<double>(block.detached->count)));
        b.add("detached", std::move(d));
    }
    return b;
}

std::string block_text(const Block& block) {
    return json::write(block_to_json(block));
}

// A Pile or a Prop block from a page saved at format 2 or earlier, turned
// into the Object that replaced both. It keeps the block's uid, so the module
// ids and seeds derived from it are the ones it had and its bodies stay where
// they were; only the controls are renamed, since Object's merge what Pile
// and Prop each called their own.
//   Pile: `shape` (heap or stack) became `arrange`, and a pile was always a
//     body the world moves, so the rest of Object's defaults are right.
//   Prop: `shape` became `collides` (its own shape reading as "own"), the
//     model moved from a file control to the model choice, and the material
//     it always carried is now an override, so `own_material` goes on.
// `layer_size` is the low end of the layer's Size block, which is the size a
// pile's bodies were born at: an Object says its size in metres, so that is
// what a converted pile asks for and nothing changes size.
void into_object(Block& block, float layer_size) {
    const bool pile = block.kind == "pile";
    const bool prop = block.kind == "prop";
    if (!pile && !prop) {
        return;
    }
    Block out;
    out.uid = block.uid;
    out.kind = "object";
    out.enabled = block.enabled;
    out.published = block.published;
    const auto carry = [&](std::string_view from, std::string_view to) {
        if (const Value* value = block.find(from)) {
            out.set(to, *value);
        }
    };
    if (pile) {
        const Value* shape = block.find("shape");
        const bool stack = shape != nullptr && shape->text == "stack";
        out.set("arrange", choice(stack ? "stack" : "heap"));
        // A pile was drawn and collided as the layer's own pieces, at the
        // size every body was born at.
        out.set("model", choice("layer"));
        out.set("size", number(layer_size));
        for (const auto& [from, to] : {std::pair{"count", "count"}, std::pair{"slope", "slope"},
                                       std::pair{"width", "width"}, std::pair{"depth", "depth"},
                                       std::pair{"release", "release"}, std::pair{"at", "at"},
                                       std::pair{"spacing", "spacing"}, std::pair{"jitter", "jitter"},
                                       std::pair{"seed", "seed"}}) {
            carry(from, to);
        }
    } else {
        const Value* shape = block.find("shape");
        const std::string collides = shape != nullptr && !shape->text.empty()
                                         ? (shape->text == "model" ? std::string{"own"} : shape->text)
                                         : std::string{"own"};
        out.set("collides", choice(collides));
        // A prop's model was a file; Object names one through its choice.
        if (const Value* model = block.find("model"); model != nullptr && !model->text.empty()) {
            out.set("model", choice("file"));
            out.set("model_file", *model);
        } else {
            out.set("model", choice("layer"));
        }
        out.set("own_material", toggle(true));
        for (const auto& [from, to] :
             {std::pair{"motion", "motion"}, std::pair{"at", "at"}, std::pair{"turn", "turn"},
              std::pair{"size", "size"}, std::pair{"wall", "wall"}, std::pair{"radius", "radius"},
              std::pair{"height", "height"}, std::pair{"extent", "extent"}, std::pair{"move_to", "move_to"},
              std::pair{"turn_to", "turn_to"}, std::pair{"move_at", "move_at"},
              std::pair{"move_over", "move_over"}, std::pair{"friction", "friction"},
              std::pair{"bounce", "bounce"}, std::pair{"density", "density"}, std::pair{"sides", "sides"}}) {
            carry(from, to);
        }
    }
    block = std::move(out);
}

std::optional<Block> block_from_json(const json::Value& b, std::string& error) {
    Block block;
    block.uid = static_cast<Uid>(std::max(b.number_or("uid", 0.0), 0.0));
    block.kind = b.string_or("kind", "");
    block.enabled = b.bool_or("enabled", true);
    const BlockDef* def = find_block_def(block.kind);
    if (def == nullptr) {
        error = "The page uses a block this Studio does not have: '" + block.kind + "'";
        return std::nullopt;
    }
    if (const json::Value* module = b.find("module"); module != nullptr) {
        std::string why;
        block.raw = raw_from_json(*module, why);
        if (!block.raw) {
            error = "A Module block could not be read: " + why;
            return std::nullopt;
        }
    }
    if (const json::Value* values = b.find("values"); values != nullptr) {
        for (const auto& [name, value] : values->members) {
            const ControlDef* control = find_control(*def, name);
            if (control == nullptr) {
                continue;
            }
            if (std::optional<Value> read = value_from(*control, value)) {
                block.set(control->id, std::move(*read));
            }
        }
    }
    if (const json::Value* published = b.find("published"); published != nullptr) {
        for (const json::Value& p : published->items) {
            const ControlDef* control = find_control(*def, p.string_or("control", ""));
            const json::Value* range = p.find("range");
            if (control == nullptr || !control->publishable || range == nullptr ||
                range->items.size() != 2U || range->items[0].type != json::Value::Type::Number ||
                range->items[1].type != json::Value::Type::Number) {
                continue;
            }
            Published entry;
            entry.control = std::string{control->id};
            entry.name = typed_parameter_name(p.string_or("name", ""));
            entry.low = static_cast<float>(range->items[0].number);
            entry.high = static_cast<float>(range->items[1].number);
            if (!entry.name.empty() && std::isfinite(entry.low) && std::isfinite(entry.high)) {
                block.publish(std::move(entry));
            }
        }
    }
    if (const json::Value* d = b.find("detached"); d != nullptr && block.raw) {
        const json::Value* original = d->find("block");
        const json::Value* module = d->find("module");
        std::string why;
        std::optional<RawModule> as_detached = module != nullptr ? raw_from_json(*module, why) : std::nullopt;
        if (original != nullptr && original->type == json::Value::Type::Object && as_detached) {
            block.detached = Detached{static_cast<Uid>(std::max(d->number_or("from", 0.0), 0.0)), json::write(*original),
                                      std::move(*as_detached),
                                      static_cast<std::uint32_t>(std::clamp(d->number_or("count", 1.0), 1.0, 1.0e6))};
        }
    }
    return block;
}

std::optional<Block> block_from_text(std::string_view text, std::string& error) {
    json::ParseError parse_error;
    const std::optional<json::Value> value = json::parse(text, parse_error);
    if (!value) {
        error = "Not a readable block: " + parse_error.message;
        return std::nullopt;
    }
    return block_from_json(*value, error);
}

std::string save_document(const Document& document) {
    json::Value root = json::Value::object();
    root.add("format", json::Value::of("ajfx-studio"));
    root.add("version", json::Value::of(static_cast<double>(document_version)));
    root.add("name", json::Value::of(document.name));
    root.add("scale", json::Value::of(static_cast<double>(document.scale)));
    root.add("loop_seconds", json::Value::of(static_cast<double>(document.loop_seconds)));
    root.add("loop_auto", json::Value::of(document.loop_auto));
    root.add("backdrop", json::Value::of(std::string{backdrop_name(document.backdrop)}));
    root.add("glow", json::Value::of(static_cast<double>(document.glow)));
    root.add("next_uid", json::Value::of(static_cast<double>(document.next_uid)));
    json::Value layers = json::Value::array();
    for (const Layer& layer : document.layers) {
        json::Value out = json::Value::object();
        out.add("uid", json::Value::of(static_cast<double>(layer.uid)));
        out.add("name", json::Value::of(layer.name));
        out.add("enabled", json::Value::of(layer.enabled));
        out.add("solo", json::Value::of(layer.solo));
        out.add("kind", json::Value::of(std::string{layer_kind_id(layer.kind)}));
        if (layer.world) {
            out.add("world", block_to_json(*layer.world));
        }
        json::Value life = json::Value::array();
        life.push(json::Value::of(static_cast<double>(layer.life_min)));
        life.push(json::Value::of(static_cast<double>(layer.life_max)));
        out.add("life", std::move(life));
        if (layer.seed != 0U) {
            out.add("seed", json::Value::of(static_cast<double>(layer.seed)));
        }
        json::Value blocks = json::Value::array();
        for (const Block& block : layer.blocks) {
            blocks.push(block_to_json(block));
        }
        out.add("blocks", std::move(blocks));
        layers.push(std::move(out));
    }
    root.add("layers", std::move(layers));
    return json::write(root);
}

Loaded load_document(std::string_view text) {
    Loaded loaded;
    json::ParseError parse_error;
    const std::optional<json::Value> root = json::parse(text, parse_error);
    if (!root) {
        loaded.error = "Not a readable page: " + parse_error.message + " (at byte " +
                       std::to_string(parse_error.offset) + ")";
        return loaded;
    }
    if (root->string_or("format", "") != "ajfx-studio") {
        loaded.error = "This file is not an ajfx Studio page";
        return loaded;
    }
    const double version = root->number_or("version", 0.0);
    if (version > document_version || version < 1.0) {
        loaded.error = "This page was saved by a newer ajfx Studio (format " + std::to_string(static_cast<int>(version)) +
                       ")";
        return loaded;
    }
    Document document;
    document.name = root->string_or("name", document.name);
    document.scale = std::clamp(static_cast<float>(root->number_or("scale", 1.0)), 0.01F, 100.0F);
    document.loop_seconds = std::clamp(static_cast<float>(root->number_or("loop_seconds", 6.0)), 0.5F, 120.0F);
    document.loop_auto = root->bool_or("loop_auto", true);
    document.backdrop = backdrop_from(root->string_or("backdrop", "night"));
    document.glow = std::clamp(static_cast<float>(root->number_or("glow", 0.6)), 0.0F, 8.0F);
    Uid highest = 0U;
    const auto uid_of = [&](const json::Value& value) {
        const Uid uid = static_cast<Uid>(std::max(value.number_or("uid", 0.0), 0.0));
        highest = std::max(highest, uid);
        return uid;
    };
    if (const json::Value* layers = root->find("layers"); layers != nullptr) {
        for (const json::Value& in : layers->items) {
            Layer layer;
            layer.uid = uid_of(in);
            layer.name = in.string_or("name", "Layer");
            layer.enabled = in.bool_or("enabled", true);
            layer.solo = in.bool_or("solo", false);
            if (const json::Value* life = in.find("life");
                life != nullptr && life->items.size() == 2U && life->items[0].type == json::Value::Type::Number &&
                life->items[1].type == json::Value::Type::Number) {
                layer.life_min = std::max(static_cast<float>(life->items[0].number), 0.01F);
                layer.life_max = std::max(static_cast<float>(life->items[1].number), layer.life_min);
            }
            layer.seed = static_cast<std::uint32_t>(std::clamp(in.number_or("seed", 0.0), 0.0, 1.0e6));
            // A page saved before layers had a kind says nothing. Its kind
            // is what its blocks were: a Physics, Pile or Prop block put
            // the whole layer in a physics world, which is the confusion
            // the kind exists to end.
            const json::Value* stated = in.find("kind");
            bool infer = stated == nullptr || stated->type != json::Value::Type::String;
            layer.kind = infer ? LayerKind::Particles : layer_kind_from(stated->text);
            if (const json::Value* blocks = in.find("blocks"); blocks != nullptr) {
                for (const json::Value& b : blocks->items) {
                    std::string error;
                    std::optional<Block> block = block_from_json(b, error);
                    if (!block) {
                        loaded.error = std::move(error);
                        return loaded;
                    }
                    highest = std::max(highest, block->uid);
                    if (block->detached) {
                        highest = std::max(highest, block->detached->from);
                    }
                    if (infer && (block->kind == world_block_kind || block->kind == "pile" || block->kind == "prop")) {
                        layer.kind = LayerKind::Rigid;
                    }
                    layer.blocks.push_back(std::move(*block));
                }
            }
            if (const json::Value* world = in.find("world"); world != nullptr && layer.kind == LayerKind::Rigid) {
                std::string error;
                if (std::optional<Block> block = block_from_json(*world, error);
                    block && block->kind == world_block_kind) {
                    highest = std::max(highest, block->uid);
                    layer.world = std::move(block);
                }
            }
            if (layer.kind == LayerKind::Rigid && !layer.world) {
                // Either an older page, whose world was the Physics block on
                // its list, or a rigid layer that never had one: the block
                // leaves the list and becomes the layer's own settings.
                const auto found = std::find_if(layer.blocks.begin(), layer.blocks.end(),
                                                [](const Block& b) { return b.kind == world_block_kind; });
                if (found != layer.blocks.end()) {
                    layer.world = std::move(*found);
                    layer.blocks.erase(found);
                } else {
                    Block world_block;
                    world_block.kind = std::string{world_block_kind};
                    layer.world = std::move(world_block);
                }
            }
            // Any further Physics block is a duplicate of the world it just
            // became; the layer has one world and holds it once.
            std::erase_if(layer.blocks, [](const Block& b) { return b.kind == world_block_kind; });
            // Pile and Prop became Object. The layer's Size is read first,
            // since a converted pile asks for the size its bodies were born
            // at and an Object says its size in metres.
            float layer_size = 1.0F;
            for (const Block& block : layer.blocks) {
                if (block.kind == "size") {
                    if (const Value* value = block.find("size")) {
                        layer_size = std::max(value->x, 1.0e-4F);
                    } else if (const BlockDef* def = find_block_def("size")) {
                        if (const ControlDef* control = find_control(*def, "size")) {
                            layer_size = std::max(control->fallback.x, 1.0e-4F);
                        }
                    }
                }
            }
            for (Block& block : layer.blocks) {
                into_object(block, layer_size);
            }
            for (Block& block : layer.blocks) {
                conform_block(block, layer.kind);
            }
            document.layers.push_back(std::move(layer));
        }
    }
    // Every uid must be unique for module ids to be; a hand-edited page that
    // repeats one is renumbered from the top.
    std::vector<Uid> seen;
    bool repeated = false;
    for (const Layer& layer : document.layers) {
        repeated = repeated || layer.uid == 0U || std::find(seen.begin(), seen.end(), layer.uid) != seen.end();
        seen.push_back(layer.uid);
        for (const Block& block : layer.blocks) {
            repeated = repeated || block.uid == 0U || std::find(seen.begin(), seen.end(), block.uid) != seen.end();
            seen.push_back(block.uid);
        }
        // A world lifted out of an older page's blocks keeps that block's
        // uid, so its random streams and module ids stay where they were;
        // one made fresh has none yet and is given one below.
        if (layer.world && layer.world->uid != 0U) {
            repeated = repeated || std::find(seen.begin(), seen.end(), layer.world->uid) != seen.end();
            seen.push_back(layer.world->uid);
        }
    }
    document.next_uid = std::max<Uid>(static_cast<Uid>(root->number_or("next_uid", 1.0)), highest + 1U);
    if (repeated) {
        document.next_uid = 1U;
        for (Layer& layer : document.layers) {
            layer.uid = document.allocate();
            if (layer.world) {
                layer.world->uid = document.allocate();
            }
            for (Block& block : layer.blocks) {
                block.uid = document.allocate();
            }
        }
    }
    for (Layer& layer : document.layers) {
        if (layer.world && layer.world->uid == 0U) {
            layer.world->uid = document.allocate();
        }
    }
    loaded.document = std::move(document);
    return loaded;
}

bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& error) {
    // Written beside the target and renamed over it, so a reader never sees
    // half a file.
    std::filesystem::path temporary = path;
    temporary += ".partial";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) {
            error = "Could not write " + path.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            error = "Could not write " + path.string();
            return false;
        }
    }
    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "Could not replace " + path.string() + " (it may be open elsewhere)";
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        error = "Could not open " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return bytes;
}

} // namespace ajfx::studio
