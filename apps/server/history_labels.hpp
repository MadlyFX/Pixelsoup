#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/editor/history.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <string>

namespace mediaserver::app {

// The undo contexts the editor navigates. Other contexts (hardware, feed map,
// switcher, link publication) keep their own history out of the operator's.
inline constexpr cmd::UndoContextId scene_undo_context{2U};
inline constexpr cmd::UndoContextId compositor_undo_context{6U};

[[nodiscard]] constexpr cmd::UndoContextId
undo_context_for(editor::HistoryContext context) noexcept {
    return context == editor::HistoryContext::Compositor
               ? compositor_undo_context
               : scene_undo_context;
}

// Describes one retained history step for the operator from its commands
// alone, so no authoring site has to name its edit. `applied` says which way
// the commands run: an undone step holds the generated undo, whose forward
// payloads restore the state before the original edit.
[[nodiscard]] editor::HistoryStep describe_history_step(
    std::span<const cmd::Command> commands, bool applied);

// The Director's presentation of each navigable context. It remembers each
// step's description until the step's commands change, and holds names for
// composite actions whose commands alone read badly, such as a paste. Nothing
// here is replicated or persisted.
class HistoryDescriptions final {
public:
    // Names the step first authored by `transaction`. Call after the edit was
    // accepted and before the next describe() of that context; the name then
    // follows its step through undo and redo.
    void name(cmd::UndoContextId context, cmd::TransactionId transaction,
              std::string label);

    // Rebuilds one context's presentation from the scheduler. Call only when
    // the context's history revision has changed: steps whose commands are
    // unchanged reuse their earlier description.
    [[nodiscard]] editor::HistoryContextState describe(
        const cmd::DirectorScheduler& scheduler, cmd::UndoContextId context);

private:
    struct Key final {
        cmd::UndoContextId context;
        cmd::TransactionId transaction;

        auto operator<=>(const Key&) const noexcept = default;
    };
    struct StepKey final {
        cmd::UndoContextId context;
        cmd::HistoryStepId step;

        auto operator<=>(const StepKey&) const noexcept = default;
    };
    struct Described final {
        cmd::TransactionId transaction;
        std::size_t commands{};
        editor::HistoryStep step;
    };

    // Names registered before their step was observed in a view.
    std::map<Key, std::string> pending_;
    std::map<StepKey, std::string> names_;
    std::map<StepKey, Described> described_;
};

} // namespace mediaserver::app
