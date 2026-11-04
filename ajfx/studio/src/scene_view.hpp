#pragma once
#include "scene_properties.hpp"

#include <imgui.h>

#include <set>
#include <string_view>
#include <vector>

namespace ajfx::studio {
// Whether the inspector shows `p` for `item`: a property that does not apply
// to what the item is set to is left out.
[[nodiscard]] bool property_visible(const SceneItem &item, const SceneProperty &p);

// The scene's tree and inspector, in Dear ImGui windows. All view state stays
// here; it never changes a scene's solver membership.
class SceneView final {
  public:
    // The panels are windows like any other: where they are is the dock's.
    void tree_panel(Document &document);
    void inspector_panel(Document &document, std::string_view issue);
    // Where the tree drew each item last frame (left, top, right, bottom).
    [[nodiscard]] const std::vector<std::pair<Uid, ImVec4>> &tree_rows() const { return tree_rows_; }
    void select(Uid uid, bool extend = false);
    void forget();
    [[nodiscard]] Uid selected() const { return selection_.empty() ? 0U : selection_.back(); }
    [[nodiscard]] const std::vector<Uid> &selection() const { return selection_; }
    // A value is being dragged or typed: the edit is one gesture, for undo.
    [[nodiscard]] bool gesture_live() const { return im_live_; }
    void import_models(Document &document);
    // Opens the Add menu the next time the tree is drawn.
    void request_add() { add_requested_ = true; }
    // What the scene costs, as lines for Scene settings; the host counts them.
    void set_statistics(std::vector<std::string> lines) { statistics_ = std::move(lines); }
    void restore(const std::vector<Uid> &selected, const std::vector<Uid> &folded) {
        selection_ = selected;
        folded_ = std::set<Uid>{folded.begin(), folded.end()};
    }
    [[nodiscard]] std::vector<Uid> folded() const { return {folded_.begin(), folded_.end()}; }

  private:
    void im_settings(Document &document);
    void im_relationships(Document &document, const SceneItem *item);
    void im_sources(Document &document, SceneItem &item);
    void im_grid_cells(const SceneItem &item);
    void im_targets(Document &document, SceneItem &item);
    void im_motion_stack(Document &document, SceneItem &item);
    void im_over_age(SceneItem &item);
    bool im_live_{};
    bool add_requested_{};
    std::vector<std::pair<Uid, ImVec4>> tree_rows_;
    std::vector<Uid> selection_;
    std::set<Uid> folded_;
    // The selection the inspector last showed, so another one scrolls to the top.
    std::vector<Uid> inspected_;
    std::vector<std::string> statistics_;
};
} // namespace ajfx::studio
