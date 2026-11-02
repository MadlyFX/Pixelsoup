#pragma once

#include "bake.hpp"
#include "budget.hpp"
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
    // Each layer's bake state, for its heading.
    const BakeStatus* bake{nullptr};
    // "Maximize budget": under way, or what it last came to.
    const BudgetView* budget{nullptr};
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

// What the artist is working on. The block last opened or changed and the
// layer picked by its heading are what the preview's guides and paths follow,
// and only one of the two stands at a time. The layer last worked on outlives
// both, so a click that adds a block has somewhere to add it once Esc or a
// fold has cleared them.
struct Selection final {
    // The block last opened or changed; 0 for none.
    Uid block{};
    // The layer picked by its heading, while no block is; 0 for none.
    Uid layer{};
    // The layer last worked on; 0 until one has been.
    Uid last_layer{};

    // Esc: nothing worked on. The last layer worked on is remembered.
    void clear_working() noexcept { block = layer = 0U; }
    // A block of `owner` was opened or changed.
    void work_on_block(Uid owner, Uid uid) noexcept {
        block = uid;
        layer = 0U;
        last_layer = owner;
    }
    // A layer was picked by its heading. The block, if it is in another
    // layer, is the caller's to let go of.
    void pick_layer(Uid uid) noexcept { layer = last_layer = uid; }
    // The layer whose guides the page is showing, or the last one picked.
    [[nodiscard]] Uid picked_layer() const noexcept { return layer != 0U ? layer : last_layer; }
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
    // The layer a palette click or "/" adds to, which the page marks and the
    // palette names: the one being worked on, else the last one worked on,
    // else the last on the page; 0 on an empty page. One rule, so what is
    // marked is what is added to.
    [[nodiscard]] Uid target_layer(const Document& document) const;
    // A palette entry is held over the window this frame (null when none):
    // the page shows where it would land and adds it there when it is let go
    // over the page.
    void set_incoming(const Incoming* incoming) noexcept { incoming_ = incoming; }
    // A palette entry clicked: added at the end of the working layer (a
    // block) or the page (a layer).
    void add(Document& document, const Incoming& incoming);
    void open_block(Uid uid) {
        open_.insert(uid);
        selection_.work_on_block(selection_.last_layer, uid);
    }
    // What a click in the preview picked or made: the block opened and
    // worked on, its layer unfolded, so its handles are the ones offered.
    void select_block(const Document& document, Uid block);
    // A layer picked with no block of its own (a piece of something broken):
    // worked on by its heading.
    void select_layer(Uid layer);
    // The block last opened or changed, whose guides the preview shows
    // strongly and whose handles it offers; 0 for none.
    [[nodiscard]] Uid focus() const noexcept { return selection_.block; }
    // Where the layer's starter tiles - what a layer with nothing born in it
    // offers to begin with - were drawn last frame, in the order the coach
    // offers them. Empty for a layer that offers none.
    [[nodiscard]] std::vector<gui::Rect> starter_tiles(Uid layer) const;
    // Where the layer's next-step chips - what a layer with something born in
    // it offers to add next - were drawn last frame, in the order the coach
    // offers them. Empty for a layer that offers none.
    [[nodiscard]] std::vector<gui::Rect> next_step_chips(Uid layer) const;
    // Where a layer's heading chips were drawn last frame, by name: "what"
    // always, "kind" in the expert view (Advanced), and "bake" while it
    // shows (a baked layer always, a live one while the pointer is over its
    // heading or it is the target).
    [[nodiscard]] std::optional<gui::Rect> header_chip(Uid layer, std::string_view name) const;
    // Where a block's heading chip was drawn last frame, by name: "explode"
    // on an Object in a solid layer, while the pointer is over the block or
    // it is open.
    [[nodiscard]] std::optional<gui::Rect> block_chip(Uid block, std::string_view name) const;
    // Values tried for published parameters, which change only the preview.
    [[nodiscard]] const std::map<std::string, ParameterValue>& preview_values() const noexcept {
        return preview_values_;
    }
    void try_value(std::string name, ParameterValue value) { preview_values_[std::move(name)] = value; }
    // The budget row's button was pressed since last asked: maximize, scale
    // down to fit, or stop the run under way. The application measures.
    [[nodiscard]] std::optional<BudgetRequest> take_budget_request() {
        std::optional<BudgetRequest> out = budget_request_;
        budget_request_.reset();
        return out;
    }
    // Where the budget row's button was drawn last frame.
    [[nodiscard]] std::optional<gui::Rect> budget_button() const noexcept { return budget_button_; }

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
    // The page's world: its ground, gravity and wind, which every layer gets.
    float draw_world(gui::UiContext& ctx, Document& document, float x, float y, float w);
    // The page's frame-time budget, and "Maximize budget".
    float draw_budget(gui::UiContext& ctx, Document& document, const PageInputs& inputs, float x, float y, float w);
    float draw_layer(gui::UiContext& ctx, Document& document, std::size_t layer_index, const PageInputs& inputs,
                     float x, float y, float w);
    float draw_layer_header(gui::UiContext& ctx, Document& document, Layer& layer, const PageInputs& inputs, float x,
                            float y, float w);
    float draw_layer_settings(gui::UiContext& ctx, Layer& layer, float x, float y, float w);
    float draw_add_block(gui::UiContext& ctx, const Document& document, Layer& layer, float x, float y, float w);
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
    // Opens `block` and makes it, and its layer, the one being worked on.
    void work_on(const Layer& layer, Uid block);
    // What a layer with nothing born in it offers to begin with, as tiles to
    // click. Nothing for a layer that has something born.
    float draw_starters(gui::UiContext& ctx, const Layer& layer, float x, float y, float w);
    // What a layer with something born in it offers to add next, as chips in
    // a row under its blocks: a look, a fade, a ground. Nothing when there is
    // nothing to suggest.
    float draw_next_steps(gui::UiContext& ctx, const Document& document, const Layer& layer, float x, float y,
                          float w);
    // Adds the blocks of a suggestion to the end of the layer, and opens the
    // first.
    void take_suggestion(Document& document, Uid layer, const Suggestion& suggestion);
    // "Explode this" on an Object of a solid layer (0: its first), once the
    // page has drawn; the Explosion it sets off is opened and worked on.
    void explode(Uid layer, Uid object);
    // The same, at once: for a menu, which acts after the page has drawn.
    void explode_now(Document& document, Uid layer, Uid object);
    // An Object's "Made of" changed: its material goes into it and its Break.
    void material_changed(Uid object);
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
    Selection selection_;
    // The layer a click adds to, as of this frame's start (`target_layer`).
    Uid target_{};
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
    // The layer whose kind chip opened the menu of kinds.
    Uid menu_kind_{};
    // The layer whose chip opened the menu of what it is.
    Uid menu_what_{};
    gui::Rect page_{};
    std::string layer_name_;
    // The page's solid layers this frame, by uid and name, for a control
    // that names one (ControlType::SolidLayer).
    std::vector<std::pair<Uid, std::string>> solid_layers_;
    std::map<std::string, ParameterValue> preview_values_;
    // The budget row: what its button asked for, where it was, and whether
    // particles shrink as there are more.
    std::optional<BudgetRequest> budget_request_;
    std::optional<gui::Rect> budget_button_;
    bool keep_look_{true};
    std::vector<std::function<void(Document&)>> deferred_;
    // The starter tiles and next-step chips drawn this frame, by layer.
    std::vector<std::pair<Uid, gui::Rect>> starter_tiles_;
    std::vector<std::pair<Uid, gui::Rect>> next_step_chips_;
    // A heading chip drawn this frame: its layer, its name and where.
    struct HeaderChip final {
        Uid layer{};
        std::string name;
        gui::Rect rect;
    };
    std::vector<HeaderChip> header_chips_;
    // A block heading's chip drawn this frame: its block, its name and where.
    struct BlockChip final {
        Uid block{};
        std::string name;
        gui::Rect rect;
    };
    std::vector<BlockChip> block_chips_;

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
