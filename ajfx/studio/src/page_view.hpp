#pragma once

#include "coach.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "package_builder.hpp"
#include "session.hpp"
#include "widgets.hpp"

#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/form_layout.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The page: an effect as a document. A title, a line of settings, then each
// layer as a heading over its blocks. A block reads as one line - what it is
// and what it is set to - and opens to its controls; it is dragged by its
// grip, added from a searchable picker (or "/"), and every value carries a
// reset arrow while it differs from its default. The coach's notes sit under
// what they are about, with their fix beside them.
namespace ajfx::studio {

struct PageInputs final {
    const Compiled* compiled{nullptr};
    const std::vector<Advice>* advice{nullptr};
    const Analysis* analysis{nullptr};
    // Shows what each block compiles to.
    bool expert{false};
    // The loop the effect plays out in, for showing beside an automatic one.
    float natural_loop{0.0F};
};

// Something from the palette, being dragged onto the page or clicked.
struct Incoming final {
    enum class Kind : std::uint8_t {
        Block,  // a catalog block, by kind
        Module, // a raw module, by its kind
        Layer,  // a ready-made layer by preset id, or "" for an empty one
    };
    Kind kind{Kind::Block};
    std::string id;
    std::string title;
    std::optional<Category> category;
};

class PageView final {
public:
    void draw(gui::UiContext& ctx, const gui::Rect& bounds, Document& document, const PageInputs& inputs);
    // Floating parts - the picker, menus, the block being dragged - drawn
    // after every panel.
    void draw_overlays(gui::UiContext& ctx, const gui::Rect& window, Document& document, const PageInputs& inputs);
    // The picker is open and over this point: the host blocks the pointer for
    // everything it draws under the card, which is drawn last.
    [[nodiscard]] bool popover_covers(float x, float y) const noexcept;

    // A drag, scrub or rename is under way: hold off recording an undo step.
    [[nodiscard]] bool gesture_live() const noexcept;
    // "/": the block picker for the layer last worked on.
    void open_block_picker(const Document& document);
    // A different page was opened: forget what was open and selected.
    void forget();
    // How the page is laid out, to keep for next time, and back again.
    [[nodiscard]] PageViewState view_state() const;
    void restore(const PageViewState& view);
    // The layer being worked on: the focused block's, or the layer last
    // picked by its heading; 0 for none. The preview's guides and paths
    // follow it.
    [[nodiscard]] Uid working_layer(const Document& document) const;
    // A palette entry is held over the window this frame (null when none):
    // the page shows where it would land and adds it there when it is let go
    // over the page.
    void set_incoming(const Incoming* incoming) noexcept { incoming_ = incoming; }
    // A palette entry clicked: added at the end of the working layer (a
    // block) or the page (a layer).
    void add(Document& document, const Incoming& incoming);
    void open_block(Uid uid) {
        open_.insert(uid);
        focus_ = uid;
        layer_focus_ = 0U;
    }
    // The block last opened or changed, whose guides the preview shows
    // strongly and whose handles it offers; 0 for none.
    [[nodiscard]] Uid focus() const noexcept { return focus_; }
    // Values tried for published parameters, which change only the preview.
    [[nodiscard]] const std::map<std::string, ParameterValue>& preview_values() const noexcept {
        return preview_values_;
    }
    void try_value(std::string name, ParameterValue value) { preview_values_[std::move(name)] = value; }

private:
    // Where a dragged block may land: the gap above `rect`, which puts the
    // block at `index` of `layer`. A rigid layer draws its blocks under
    // scope headings, so a gap belongs to one heading and only a block of
    // that scope may land in it; `tail` is the gap at a heading's foot,
    // which is where a block of its scope goes when nothing follows it.
    struct Slot final {
        Uid layer{};
        std::size_t index{};
        gui::Rect rect;
        RigidScope scope{RigidScope::Pieces};
        bool tail{false};
    };
    struct LayerArea final {
        Uid layer{};
        gui::Rect rect;
        std::size_t count{};
        float end_y{};
        // Drawn under scope headings (a rigid layer), so a drop is held to
        // the heading the dragged block belongs under.
        bool grouped{false};
    };
    enum class PickerMode {
        Blocks,
        Modules,
        Layers,
    };

    float draw_header(gui::UiContext& ctx, Document& document, const PageInputs& inputs, float x, float y, float w);
    float draw_layer(gui::UiContext& ctx, Document& document, std::size_t layer_index, const PageInputs& inputs,
                     float x, float y, float w);
    float draw_block(gui::UiContext& ctx, Document& document, Layer& layer, std::size_t index,
                     const PageInputs& inputs, float x, float y, float w);
    // A rigid layer's blocks under their scope headings; a particle layer's
    // as one list.
    float draw_layer_blocks(gui::UiContext& ctx, Document& document, Layer& layer, const PageInputs& inputs,
                            float x, float y, float w);
    float draw_scope_heading(gui::UiContext& ctx, RigidScope scope, float x, float y, float w);
    // The layer's own world row ("Bodies"), which reads under The world.
    float draw_world_row(gui::UiContext& ctx, Layer& layer, float x, float y, float w);
    float block_body_height(const Block& block, const BlockDef& def, LayerKind kind) const;
    float draw_controls(gui::UiContext& ctx, Block& block, const BlockDef& def, LayerKind kind, float x, float y,
                        float w);
    float draw_raw_fields(gui::UiContext& ctx, Block& block, float x, float y, float w);
    bool draw_control(gui::UiContext& ctx, Block& block, const ControlDef& control, LayerKind kind,
                      const gui::Rect& row);
    [[nodiscard]] Published default_publication(const Block& block, const ControlDef& control) const;
    float draw_publication(gui::UiContext& ctx, Block& block, const ControlDef& control, const Published& published,
                           float x, float y, float w);
    float draw_parameters(gui::UiContext& ctx, const PageInputs& inputs, float x, float y, float w);
    float draw_advice(gui::UiContext& ctx, const Advice& advice, std::size_t index, float x, float y, float w);
    float draw_hood(gui::UiContext& ctx, const std::vector<std::string>& lines, float x, float y, float w);
    bool number(gui::UiContext& ctx, gui::UiId id, const gui::Rect& field, float& value, float speed, int decimals,
                std::string_view axis = {}, float min = 0.0F, float max = 0.0F);

    // Where a block of `scope` would land in `area`: its index in the
    // layer's list and where the drop line goes, or nothing for the
    // layer's end.
    [[nodiscard]] std::optional<std::pair<std::size_t, float>> drop_gap(const LayerArea& area, RigidScope scope,
                                                                       float py) const;
    void open_picker(PickerMode mode, Uid layer, std::size_t index, float x, float y, const Document& document);
    void resolve_drag(gui::UiContext& ctx);
    void resolve_incoming(gui::UiContext& ctx, const Document& document);
    // Puts `incoming` at `index` of `layer` (a block) or of the page (a layer).
    void insert(Document& document, const Incoming& incoming, Uid layer, std::size_t index);
    bool draw_file(gui::UiContext& ctx, Block& block, const ControlDef& control, const gui::Rect& field);

    gui::ScrollRegion scroll_;
    mediaserver::editor::NumberField numbers_;
    mediaserver::editor::InlineRename rename_{gui::ui_id("studio.layer.rename")};
    mediaserver::editor::ClickTracker clicks_;
    std::set<Uid> open_;
    std::set<Uid> details_;
    std::set<Uid> folded_;
    Uid last_layer_{};
    Uid focus_{};
    Uid layer_focus_{};
    float restore_scroll_{-1.0F};

    Uid drag_block_{};
    // The scope the dragged block belongs under, taken when the drag
    // starts: a rigid layer only offers it the gaps of its own heading,
    // since its kind decides where it is drawn and the drop line would
    // otherwise point somewhere it cannot land.
    RigidScope drag_scope_{RigidScope::Pieces};
    float drag_press_y_{};
    bool dragging_{false};
    std::vector<Slot> slots_;
    std::vector<LayerArea> layer_areas_;
    std::optional<std::pair<Uid, std::size_t>> drop_;
    float drop_y_{};
    float drop_x_{};
    float drop_w_{};

    PopoverList picker_;
    PickerMode picker_mode_{PickerMode::Blocks};
    Uid picker_layer_{};
    std::size_t picker_index_{};
    std::vector<std::string> picker_ids_;

    Uid menu_block_{};
    Uid menu_layer_{};
    gui::Rect page_{};
    std::string layer_name_;
    std::map<std::string, ParameterValue> preview_values_;
    std::vector<std::function<void(Document&)>> deferred_;

    const Incoming* incoming_{nullptr};
    // Where the held palette entry would land: a layer and index for a
    // block, an index among the layers for a layer.
    std::optional<std::pair<Uid, std::size_t>> incoming_drop_;
    float incoming_y_{};
    // The layers' tops, for dropping a layer between them.
    std::vector<std::pair<Uid, float>> layer_tops_;
    float layers_end_y_{};
    float content_x_{};
    float content_w_{};
};

} // namespace ajfx::studio
