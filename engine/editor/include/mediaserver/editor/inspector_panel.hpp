#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <array>
#include <optional>
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

// Dedicated effect editors live beside Properties as dock tabs. The
// Inspector reports navigation as an intent instead of reaching into the dock
// tree itself, keeping this headless panel independently testable.
enum class InspectorPanelRequest : core::u8 {
    Color,
    Texture,
};

// A reusable three-component colour destination for viewport sampling. The
// Inspector names the document properties; the shell owns the cross-panel
// eyedropper gesture and writes the sampled RGB values back to this target.
struct InspectorColorSampleRequest final {
    doc::NodeId node{};
    std::array<std::string, 3U> properties;
};

// What one Inspector frame reported.
struct InspectorFrame final {
    std::vector<ComponentEdit> component_edits;
    // Explicit diamond presses toggle keys at the playhead. Kept distinct from
    // a value edit so auto-key is never required to discover the control.
    std::vector<InspectorKeyEdit> key_edits;
    std::vector<NamedEdit> named_edits;
    // Present only for an Up/Down edit of one focused number field. All edits
    // emitted by that field this frame belong to this one semantic repeat.
    std::optional<NumericNudge> numeric_nudge;
    // First-class effect nodes the operator explicitly removed from the
    // selected element. Kept separate from named edits: removal erases the
    // effect and its animation channels rather than merely disabling it.
    std::vector<doc::NodeId> effect_deletes;
    // A contextual "open" button beside a structured-effect note.
    std::optional<InspectorPanelRequest> panel_request;
    // A colour well asked the shell to enter viewport eyedropper mode.
    std::optional<InspectorColorSampleRequest> color_sample_request;
    // The operator pressed Delete on the panel. The shell owns the selection,
    // so it decides what that removes.
    bool delete_requested{};
    // A field is mid-scrub or mid-type. The shell holds one gesture open for as
    // long as this is true, so a drag across a field is one undo step.
    bool editing{};
    // Observable truthfulness state for the headless panel contract. Each
    // numeric field whose compatible editable targets disagree contributes
    // one entry; the control itself renders the literal `Mixed` marker.
    std::size_t mixed_fields{};
    // A multi-edit never silently drops members. These unique node lists are
    // also rendered at the foot of the Inspector, making locked and
    // field-incompatible targets explicit to the operator.
    std::vector<doc::NodeId> skipped_locked;
    std::vector<doc::NodeId> skipped_incompatible;
};

class InspectorPanel final {
  public:
    explicit InspectorPanel(gui::UiId id) noexcept : id_(id) {}

    // Compatibility form for single-element callers and focused panel tests.
    // `also_selected` affects only the header because those callers have not
    // supplied the other snapshots; production uses the canonical span form.
    InspectorFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
                        const ElementInfo* element,
                        std::size_t also_selected = 0U,
                        std::span<const scene::ChannelKeyframes> keyframes = {},
                        core::u64 show_frame = 0U,
                        std::span<const scene::EffectInstance> effects = {});

    // Canonical multi-selection form. `selected` contains the current Scene
    // selection's snapshots and `primary` chooses the sections that are shown.
    // Every compatible unlocked member participates in value comparison and
    // in edits; ordering is preserved in the emitted edit arrays.
    InspectorFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
                        std::span<const ElementInfo> selected,
                        doc::NodeId primary,
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

    enum class FieldDomain : core::u8 {
        Transform,
        Light,
        Material,
        Compositing,
        Effect,
        // A track's raster. Whole pixels, and its own domain because the
        // numbers are a picture format rather than a scene measurement.
        Screen,
    };

    // Semantic identity of one numeric field. Named properties need a domain
    // because lights and materials intentionally reuse names such as color_r.
    // Effect ordinal distinguishes duplicate effects of the same kind.
    struct FieldBinding final {
        FieldDomain domain{FieldDomain::Transform};
        scene::TransformChannel component{};
        std::string_view property;
        scene::EffectKind effect_kind{scene::EffectKind::Brightness};
        std::size_t effect_ordinal{};
    };

    struct EditableValue final {
        doc::NodeId element{};
        doc::NodeId author{};
        double value{};
    };

    struct ValueSummary final {
        std::vector<EditableValue> targets;
        std::vector<doc::NodeId> incompatible;
        bool mixed{};
    };

    struct DragCapture final {
        gui::UiId field{};
        std::vector<EditableValue> starts;
        NumberFieldEditKind kind{NumberFieldEditKind::Absolute};
        NumberFieldNudgeDirection direction{
            NumberFieldNudgeDirection::Increase};
        gui::KeyMods modifiers{gui::KeyMods::None};
        double delta{};
    };

    // The next `height` pixels of the column, advancing the cursor past them.
    [[nodiscard]] static gui::Rect take_row(Cursor& cursor,
                                            float height) noexcept;

    void draw_header(gui::UiContext& ctx, Cursor& cursor,
                     const ElementInfo& element, std::size_t selected_count);
    void draw_transform(gui::UiContext& ctx, Cursor& cursor,
                        const ElementInfo& element, InspectorFrame& frame,
                        std::span<const scene::ChannelKeyframes> keyframes,
                        core::u64 show_frame);
    void draw_light(gui::UiContext& ctx, Cursor& cursor,
                    const ElementInfo& element, InspectorFrame& frame);
    void draw_content(gui::UiContext& ctx, Cursor& cursor,
                      const ElementInfo& element, InspectorFrame& frame);
    // A track's own section: the format of the screen it owns, and whether
    // the plane's height is tied to it.
    void draw_screen(gui::UiContext& ctx, Cursor& cursor,
                     const ElementInfo& element, InspectorFrame& frame);
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

    // A labelled row of numeric fields, one per axis letter in `axes`. Each
    // binding resolves the corresponding field across the current selection,
    // renders Mixed when needed, and emits the complete fan-out directly.
    void vector_row(gui::UiContext& ctx, Cursor& cursor,
                    std::string_view label,
                    std::span<const std::string_view> axes,
                    std::span<const float> values, float speed,
                    std::span<const FieldBinding> bindings,
                    InspectorFrame& frame,
                    bool* key_requested = nullptr,
                    bool animated = false,
                    bool keyed_here = false,
                    std::size_t id_salt = 0U);

    [[nodiscard]] std::optional<EditableValue>
    editable_value(const ElementInfo& element,
                   const FieldBinding& binding) const;
    [[nodiscard]] ValueSummary summarize(const FieldBinding& binding,
                                         InspectorFrame& frame) const;
    void emit_numeric(const FieldBinding& binding, gui::UiId field,
                      const NumberFieldEdit& edit, ValueSummary summary,
                      InspectorFrame& frame);
    void draw_skip_summary(gui::UiContext& ctx, Cursor& cursor,
                           const InspectorFrame& frame);
    [[nodiscard]] InspectorFrame draw_selection(
        gui::UiContext& ctx, const gui::Rect& bounds,
        std::span<const ElementInfo> selected, doc::NodeId primary,
        std::size_t declared_count,
        std::span<const scene::ChannelKeyframes> keyframes,
        core::u64 show_frame,
        std::span<const scene::EffectInstance> effects);

    gui::UiId id_{};
    // How far the contents are scrolled up, in pixels. Clamped to what actually
    // overflowed, which is only known after a frame has been laid out.
    float scroll_{};
    float content_height_{};

    NumberField fields_;
    // Evaluated snapshots and model spans are valid only during draw(). They
    // let every row retain the compact single-selection layout while resolving
    // its semantic binding across the canonical selection.
    std::vector<ElementInfo> selected_;
    std::span<const scene::ChannelKeyframes> keyframes_{};
    std::span<const scene::EffectInstance> effects_{};
    core::u64 show_frame_{};
    DragCapture drag_capture_{};
    std::vector<doc::NodeId> selection_signature_;
    doc::NodeId primary_signature_{};

    // Which sections are open. Rebuilt from nothing on the first frame, so the
    // panel opens showing the transform, which is what an operator wants first.
    bool transform_open_{true};
    bool light_open_{true};
    bool material_open_{true};
    bool content_open_{true};
    bool screen_open_{true};
    bool compositing_open_{true};
    bool effects_open_{true};
};

} // namespace mediaserver::editor
