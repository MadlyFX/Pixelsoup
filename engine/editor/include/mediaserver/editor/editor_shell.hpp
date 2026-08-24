#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <memory>
#include <string>
#include <vector>

// The operator editor: a Dear ImGui surface for placing scene elements.
//
// This is the throwaway shell of §9's plan - a real but disposable UI, isolated
// in this module so the bespoke operator surface can replace it without
// touching anything else. It exists to prove the interaction model: the 2D wall
// view and (later) a 3D view are two editors of the one replicated scene, and a
// drag in either resolves - through the pure math in scene::manipulate - to a
// document command.
//
// It authors nothing itself. Each frame it returns the edits the operator made
// as intents; the Director turns those into PutNode commands on the frame-
// scheduled bus, so undo, replication, and replay stay exactly as they are for
// every other edit. The editor never mutates scene or document state directly.
namespace mediaserver::editor {

enum class EditorError : core::u8 {
    // No GPU, or a device/window that could not be created.
    Unavailable,
    DisplayUnavailable,
    AllocationFailed,
};

enum class EditorStatus : core::u8 {
    Running,
    // The operator closed the editor window. The show should stop.
    Closed,
};

// One edit the operator made this frame: a node moved to a new world position.
// The Director reads the node's current transform, sets its position, and
// authors a PutNode. Position only for now; rotation and scale follow the same
// shape.
struct EditIntent final {
    doc::NodeId node{};
    scene::Vec3 position{};

    bool operator==(const EditIntent&) const = default;
};

// The result of one editor frame: whether it is still open, and the edits to
// author. Edits are already coalesced to one per node per frame.
struct EditorFrame final {
    EditorStatus status{EditorStatus::Running};
    std::vector<EditIntent> edits;
};

struct EditorConfig final {
    std::string title{"media-server editor"};
    core::u32 width{1600U};
    core::u32 height{900U};
};

// True when this build can open the editor: it has the scene shader bytecode and
// a GPU to run it on. Checked once so the Director can refuse cleanly.
[[nodiscard]] bool editor_available() noexcept;

class EditorShell final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<EditorShell>, EditorError>
    create(const EditorConfig& config) noexcept;

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;
    EditorShell(EditorShell&&) = delete;
    EditorShell& operator=(EditorShell&&) = delete;
    ~EditorShell();

    // Renders the wall view of `scene`, runs one UI frame, and returns the
    // operator's edits and whether the window is still open. `scene` is the
    // committed scene evaluated at the current show frame - the same value every
    // node draws - so the editor edits exactly what the wall shows.
    [[nodiscard]] EditorFrame frame(const scene::Scene& scene) noexcept;

private:
    struct Impl;

    explicit EditorShell(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::editor
