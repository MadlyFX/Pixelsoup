#include <mediaserver/editor/async_dialog.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace editor = mediaserver::editor;
namespace core = mediaserver::core;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Stands in for the editor frame a continuation edits.
struct Context final {
  std::vector<std::string> applied;
  int confirmed{};
};

using Dialog = editor::AsyncDialog<Context>;

constexpr core::u64 show_generation = 7U;

// A dialog the test answers: show() blocks its thread until answer() is set.
struct Scripted final {
  std::shared_ptr<std::promise<std::optional<std::string>>> promise =
      std::make_shared<std::promise<std::optional<std::string>>>();
  std::shared_ptr<std::atomic<int>> shown = std::make_shared<std::atomic<int>>(0);

  [[nodiscard]] Dialog::Show show() const {
    return [promise = promise, shown = shown] {
      ++*shown;
      return promise->get_future().get();
    };
  }
  void answer(std::optional<std::string> chosen) const {
    promise->set_value(std::move(chosen));
  }
};

Dialog::Apply record() {
  return [](Context &context, std::string path) {
    context.applied.push_back(std::move(path));
  };
}

// Polls the way the frame loop does, until the answer lands.
Dialog::Adopt settle(Dialog &dialog, Context &context,
                     core::u64 now = show_generation) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const Dialog::Adopt adopted = dialog.adopt(context, now);
    if (adopted != Dialog::Adopt::Waiting) {
      return adopted;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  throw std::runtime_error{"dialog answer never adopted"};
}

void an_answer_is_applied_exactly_once() {
  Dialog dialog;
  Context context;
  const Scripted scripted;
  UI_CHECK(dialog.open(scripted.show(), record(), show_generation) ==
           Dialog::Open::Opened);
  UI_CHECK(dialog.pending());
  // Unanswered: polling neither blocks nor applies.
  UI_CHECK(dialog.adopt(context, show_generation) == Dialog::Adopt::Waiting);
  UI_CHECK(context.applied.empty());

  scripted.answer("D:/show/T009.mov");
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(context.applied.size() == 1U);
  UI_CHECK(context.applied.front() == "D:/show/T009.mov");
  UI_CHECK(!dialog.pending());
  for (int frame = 0; frame < 4; ++frame) {
    UI_CHECK(dialog.adopt(context, show_generation) == Dialog::Adopt::Waiting);
  }
  UI_CHECK(context.applied.size() == 1U);
  UI_CHECK(*scripted.shown == 1);
}

void a_cancelled_dialog_applies_nothing() {
  Dialog dialog;
  Context context;
  const Scripted scripted;
  UI_CHECK(dialog.open(scripted.show(), record(), show_generation) ==
           Dialog::Open::Opened);
  scripted.answer(std::nullopt);
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Cancelled);
  UI_CHECK(context.applied.empty());
  UI_CHECK(!dialog.pending());
}

void a_second_request_is_refused_while_one_is_open() {
  Dialog dialog;
  Context context;
  const Scripted first;
  const Scripted second;
  UI_CHECK(dialog.open(first.show(), record(), show_generation) ==
           Dialog::Open::Opened);
  UI_CHECK(dialog.open(second.show(),
                       [](Context &refused, std::string) {
                         refused.applied.emplace_back("refused");
                       },
                       show_generation) == Dialog::Open::Busy);
  UI_CHECK(dialog.confirm([] { return true; },
                          [](Context &refused) { ++refused.confirmed; },
                          show_generation) == Dialog::Open::Busy);
  first.answer("C:/first.mstage");
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(context.applied.size() == 1U);
  UI_CHECK(context.applied.front() == "C:/first.mstage");
  UI_CHECK(context.confirmed == 0);
  UI_CHECK(*second.shown == 0);

  // The slot is free again once the first answer is adopted.
  const Scripted third;
  UI_CHECK(dialog.open(third.show(), record(), show_generation) ==
           Dialog::Open::Opened);
  third.answer("C:/third.mstage");
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(context.applied.size() == 2U);
  UI_CHECK(context.applied.back() == "C:/third.mstage");
  // Never answered; the refused request must not have started a thread.
  second.answer(std::nullopt);
}

void a_confirm_runs_its_continuation_only_on_yes() {
  Dialog dialog;
  Context context;
  UI_CHECK(dialog.confirm([] { return false; },
                          [](Context &asked) { ++asked.confirmed; },
                          show_generation) == Dialog::Open::Opened);
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Cancelled);
  UI_CHECK(context.confirmed == 0);

  UI_CHECK(dialog.confirm([] { return true; },
                          [](Context &asked) { ++asked.confirmed; },
                          show_generation) == Dialog::Open::Opened);
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(context.confirmed == 1);
  UI_CHECK(dialog.adopt(context, show_generation) == Dialog::Adopt::Waiting);
  UI_CHECK(context.confirmed == 1);
}

void an_answer_for_a_replaced_project_is_dropped() {
  Dialog dialog;
  Context context;
  const Scripted scripted;
  UI_CHECK(dialog.open(scripted.show(), record(), show_generation) ==
           Dialog::Open::Opened);
  scripted.answer("D:/ies/old-project.ies");
  UI_CHECK(settle(dialog, context, show_generation + 1U) == Dialog::Adopt::Stale);
  UI_CHECK(context.applied.empty());
  UI_CHECK(!dialog.pending());
}

void a_continuation_can_open_the_next_step() {
  // Choose a project, then confirm discarding the open one, then load.
  Dialog dialog;
  Context context;
  const Scripted chooser;
  Dialog::Open next_step = Dialog::Open::Failed;
  UI_CHECK(dialog.open(chooser.show(),
                       [&dialog, &next_step](Context &, std::string path) {
                         next_step = dialog.confirm(
                             [] { return true; },
                             [path](Context &step) {
                               step.applied.push_back(path);
                             },
                             show_generation);
                       },
                       show_generation) == Dialog::Open::Opened);
  chooser.answer("E:/shows/next.mstage");
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(next_step == Dialog::Open::Opened);
  UI_CHECK(context.applied.empty());
  UI_CHECK(settle(dialog, context) == Dialog::Adopt::Applied);
  UI_CHECK(context.applied.size() == 1U);
  UI_CHECK(context.applied.front() == "E:/shows/next.mstage");
}

} // namespace

int main() {
  try {
    an_answer_is_applied_exactly_once();
    a_cancelled_dialog_applies_nothing();
    a_second_request_is_refused_while_one_is_open();
    a_confirm_runs_its_continuation_only_on_yes();
    an_answer_for_a_replaced_project_is_dropped();
    a_continuation_can_open_the_next_step();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "async dialog tests passed\n";
  return 0;
}
