#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Inspector, on the bespoke UI rather than Dear ImGui.
//
// It shows the selection's authored values - position, rotation, scale, and a
// light's own fields - and lets the operator set them. Like the viewport panel
// beside it, it authors nothing: a frame reports which properties the operator
// changed and to what, and the shell above turns that into document intents.
// It does not even name a transaction, because whether two edits belong to one
// undo step is a question about the whole gesture across every panel, and only
// the shell can see that.
//
// The rows are built from NumberField, the editor's scrubbable/typeable number:
// drag it sideways to scrub, click it to type an exact value.
namespace mediaserver::editor {

// One transform-component edit: set `component` of `node` to `value`. No
// transaction id - see above.
struct ComponentEdit final {
    doc::NodeId node{};
    scene::TransformChannel component{};
    double value{};

    bool operator==(const ComponentEdit&) const = default;
};

// A property-diamond request. A hollow diamond authors `value` at the current
// playhead; a filled diamond reports `erase` so the shell removes only that
// key, leaving the rest of the animated channel intact.
struct InspectorKeyEdit final {
    doc::NodeId node{};
    scene::TransformChannel component{};
    double value{};
    bool erase{};

    bool operator==(const InspectorKeyEdit&) const = default;
};

// One edit of a named document property - a light's colour, intensity, or
// range, which are plain properties rather than transform channels.
struct NamedEdit final {
    doc::NodeId node{};
    std::string property;
    double value{};

    bool operator==(const NamedEdit&) const = default;
};

// What one Inspector frame reported.
struct InspectorFrame final {
    std::vector<ComponentEdit> component_edits;
    // Explicit diamond presses toggle keys at the playhead. Kept distinct from
    // a value edit so auto-key is never required to discover the control.
    std::vector<InspectorKeyEdit> key_edits;
    std::vector<NamedEdit> named_edits;
    // The operator pressed Delete on the panel. The shell owns the selection,
    // so it decides what that removes.
    bool delete_requested{};
    // A field is mid-scrub or mid-type. The shell holds one gesture open for as
    // long as this is true, so a drag across a field is one undo step.
    bool editing{};
};

class InspectorPanel final {
  public:
    explicit InspectorPanel(gui::UiId id) noexcept : id_(id) {}

    // Draws the panel into `bounds`. `element` is the anchor's authored values,
    // or null for an empty selection; `also_selected` is how many others are
    // selected, shown as a "+N" beside the title so an edit that will apply to
    // one of several is not mistaken for an edit to all of them.
    InspectorFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
                        const ElementInfo* element,
                        std::size_t also_selected = 0U,
                        std::span<const scene::ChannelKeyframes> keyframes = {},
                        core::u64 show_frame = 0U,
                        std::span<const scene::EffectInstance> effects = {});

  private:
    // A vertical cursor down the panel, so a section that collapses simply
    // stops advancing it. Rebuilt every frame; nothing here survives one.
    struct Cursor final {
        // The content column: the panel inset on both sides.
        gui::Rect column{};
        float y{};
    };

    // The next `height` pixels of the column, advancing the cursor past them.
    [[nodiscard]] static gui::Rect take_row(Cursor& cursor,
                                            float height) noexcept;

    void draw_header(gui::UiContext& ctx, Cursor& cursor,
                     const ElementInfo& element, std::size_t also_selected);
    void draw_transform(gui::UiContext& ctx, Cursor& cursor,
                        const ElementInfo& element, InspectorFrame& frame,
                        std::span<const scene::ChannelKeyframes> keyframes,
                        core::u64 show_frame);
    void draw_light(gui::UiContext& ctx, Cursor& cursor,
                    const ElementInfo& element, InspectorFrame& frame);
    void draw_content(gui::UiContext& ctx, Cursor& cursor,
                      const ElementInfo& element);
    void draw_material(gui::UiContext& ctx, Cursor& cursor,
                       const ElementInfo& element, InspectorFrame& frame);
    void draw_compositing(
        gui::UiContext& ctx, Cursor& cursor, const ElementInfo& element,
        InspectorFrame& frame,
        std::span<const scene::ChannelKeyframes> keyframes,
        core::u64 show_frame);
    void draw_effects(gui::UiContext& ctx, Cursor& cursor,
                      const ElementInfo& element, InspectorFrame& frame,
                      std::span<const scene::ChannelKeyframes> keyframes,
                      core::u64 show_frame,
                      std::span<const scene::EffectInstance> effects);

    // A collapsible section header. Returns whether the section is open.
    [[nodiscard]] bool section(gui::UiContext& ctx, Cursor& cursor,
                               std::string_view title, bool& open);

    // A labelled row of numeric fields, one per axis letter in `axes`. Writes
    // the changed axis's index into `changed_axis` and its new value into
    // `changed_value`, and returns true on a frame one of them changed. At most
    // one changes per frame, since only one field can hold the pointer.
    [[nodiscard]] bool vector_row(gui::UiContext& ctx, Cursor& cursor,
                                  std::string_view label,
                                  std::span<const std::string_view> axes,
                                  std::span<const float> values, float speed,
                                  std::size_t& changed_axis,
                                  double& changed_value,
                                  bool* key_requested = nullptr,
                                  bool animated = false,
                                  bool keyed_here = false,
                                  std::size_t id_salt = 0U);

    gui::UiId id_{};
    // How far the contents are scrolled up, in pixels. Clamped to what actually
    // overflowed, which is only known after a frame has been laid out.
    float scroll_{};
    float content_height_{};

    NumberField fields_;

    // Which sections are open. Rebuilt from nothing on the first frame, so the
    // panel opens showing the transform, which is what an operator wants first.
    bool transform_open_{true};
    bool light_open_{true};
    bool material_open_{true};
    bool content_open_{true};
    bool compositing_open_{true};
    bool effects_open_{true};
};

} // namespace mediaserver::editor
