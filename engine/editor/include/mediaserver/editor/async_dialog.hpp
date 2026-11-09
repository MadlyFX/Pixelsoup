#pragma once

#include <mediaserver/core/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace mediaserver::editor {

// One modal native dialog at a time, shown on its own thread.
//
// The editor draws inside the Director's frame loop, so a dialog that blocked
// the frame thread would stop the Director sealing frames for as long as the
// operator browsed, and the catch-up burst afterwards can push render nodes
// out of lockstep. Here `show` runs on a detached thread instead; the frame
// thread polls with adopt() and runs `apply` with the operator's answer on the
// first frame that sees it. A dialog the operator cancels applies nothing.
//
// A dialog's answer belongs to the project that asked for it: when the
// project generation changed while it was open, the answer is dropped rather
// than applied to nodes of a different document. A continuation may open the
// next dialog of a multi-step flow from inside `apply`, because the slot is
// already free by then.
template <class Context> class AsyncDialog final {
public:
  // Returns the chosen path or text; nullopt means the operator cancelled.
  using Show = std::function<std::optional<std::string>()>;
  using Apply = std::function<void(Context &, std::string)>;
  // Returns whether the operator answered yes.
  using Ask = std::function<bool()>;
  using Confirm = std::function<void(Context &)>;

  enum class Open : core::u8 { Opened, Busy, Failed };
  enum class Adopt : core::u8 { Waiting, Applied, Cancelled, Stale };

  [[nodiscard]] bool pending() const noexcept { return pending_.has_value(); }

  [[nodiscard]] Open open(Show show, Apply apply, core::u64 generation) {
    if (pending_.has_value()) {
      return Open::Busy;
    }
    auto answer = std::make_shared<Answer>();
    try {
      // Detached: the editor never waits for the operator to close it. A
      // dialog still open at shutdown only writes to its own shared answer.
      std::thread{[answer, show = std::move(show)] {
        std::optional<std::string> chosen;
        try {
          chosen = show();
        } catch (...) {
          chosen.reset();
        }
        const std::lock_guard guard{answer->mutex};
        answer->chosen = std::move(chosen);
        answer->done = true;
      }}.detach();
    } catch (...) {
      return Open::Failed;
    }
    pending_ = Pending{std::move(answer), std::move(apply), generation};
    return Open::Opened;
  }

  // A yes/no question: `yes` runs only when the operator answers yes.
  [[nodiscard]] Open confirm(Ask ask, Confirm yes, core::u64 generation) {
    return open(
        [ask = std::move(ask)]() -> std::optional<std::string> {
          return ask() ? std::optional<std::string>{std::string{}}
                       : std::nullopt;
        },
        [yes = std::move(yes)](Context &context, std::string) {
          if (yes) {
            yes(context);
          }
        },
        generation);
  }

  // Never blocks: an answer still being written is picked up next frame.
  Adopt adopt(Context &context, core::u64 generation) {
    if (!pending_.has_value()) {
      return Adopt::Waiting;
    }
    std::optional<std::string> chosen;
    {
      Answer &answer = *pending_->answer;
      const std::unique_lock guard{answer.mutex, std::try_to_lock};
      if (!guard.owns_lock() || !answer.done) {
        return Adopt::Waiting;
      }
      chosen = std::move(answer.chosen);
    }
    Pending done = std::move(*pending_);
    pending_.reset();
    if (!chosen.has_value()) {
      return Adopt::Cancelled;
    }
    if (done.generation != generation) {
      return Adopt::Stale;
    }
    if (done.apply) {
      done.apply(context, std::move(*chosen));
    }
    return Adopt::Applied;
  }

private:
  struct Answer final {
    std::mutex mutex;
    bool done{};
    std::optional<std::string> chosen;
  };
  struct Pending final {
    std::shared_ptr<Answer> answer;
    Apply apply;
    core::u64 generation{};
  };
  std::optional<Pending> pending_;
};

} // namespace mediaserver::editor
