#pragma once
#include "scene_properties.hpp"
#include "widgets.hpp"
#include <mediaserver/editor/number_field.hpp>

#include <set>

namespace ajfx::studio {
// All view state stays here; it never changes a scene's solver membership.
class SceneView final {
  public:
    void draw_tree(gui::UiContext &ctx, const gui::Rect &bounds, Document &document);
    void draw_inspector(gui::UiContext &ctx, const gui::Rect &bounds, Document &document, std::string_view issue);
    void draw_overlays(gui::UiContext &ctx, const gui::Rect &window, Document &document);
    void select(Uid uid, bool extend = false);
    void forget();
    [[nodiscard]] Uid selected() const { return selection_.empty() ? 0U : selection_.back(); }
    [[nodiscard]] bool covers(float x, float y) const {
        return add_.covers(x, y) || behavior_add_.covers(x, y) || relation_add_.covers(x, y);
    }
    [[nodiscard]] const std::vector<Uid> &selection() const { return selection_; }
    [[nodiscard]] bool gesture_live() const { return numbers_.live() || color_live_; }
    void import_models(Document &document);
    void open_add(float x, float y);
    // What the scene costs, as lines for Scene settings; the host counts them.
    void set_statistics(std::vector<std::string> lines) { statistics_ = std::move(lines); }
    void restore(const std::vector<Uid> &selected, const std::vector<Uid> &folded) {
        selection_ = selected;
        folded_ = std::set<Uid>{folded.begin(), folded.end()};
    }
    [[nodiscard]] std::vector<Uid> folded() const { return {folded_.begin(), folded_.end()}; }

  private:
    void tree_children(gui::UiContext &ctx, Document &document, Uid parent, float x, float &y, float width,
                       unsigned depth);
    void draw_sources(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y, float width);
    // The rules binding objects to particle systems: those touching `item`, or every one for null.
    void draw_relationships(gui::UiContext &ctx, Document &document, const SceneItem *item, float x, float &y,
                            float width);
    // A force's targets: a switch per particle system, when it does not act on every one.
    void draw_targets(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y, float width);
    // The Motion stack: typed behaviors in the order their forces run.
    void draw_motion_stack(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y, float width);
    // Size and colour over age: a driver choice, then editable keys.
    void draw_over_age(gui::UiContext &ctx, SceneItem &item, float x, float &y, float width);
    void draw_settings(gui::UiContext &ctx, Document &document, float x, float &y, float width);
    std::vector<Uid> selection_;
    std::set<Uid> folded_;
    gui::ScrollRegion tree_scroll_;
    gui::ScrollRegion inspector_scroll_;
    PopoverList add_;
    PopoverList behavior_add_;
    Uid behavior_target_{};
    PopoverList relation_add_;
    Uid relation_target_{};
    bool relation_from_system_{};
    std::vector<std::pair<RelationKind, Uid>> relation_choices_;
    std::vector<std::string> statistics_;
    mediaserver::editor::NumberField numbers_;
    bool color_live_{};
};
} // namespace ajfx::studio
