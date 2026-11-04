#pragma once

#include "compile.hpp"
#include "document.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

// Compiles the scene on a thread of its own.
//
// A scene changes on every frame of a drag, and a compile of a large one
// takes milliseconds in Debug: on the interface thread, every frame of the
// drag would pay it. Here the interface thread hands the scene over and
// carries on, and takes the compile up when it is done, a frame or two later.
// - **Newest wins.** A page handed over while another compiles replaces any
//   that was waiting, so a drag compiles at most one page behind the
//   pointer, never a queue of them.
// - **The fence.** Each page carries its revision (PageRevision), and a
//   result is handed out only if it is newer than every one handed out
//   before and than any `fence`, so a slow compile of an older page can
//   never replace a newer one -- the interface's own, made when a page is
//   opened, among them.
namespace ajfx::studio {

class CompileWorker final {
public:
    struct Result final {
        std::uint64_t revision{0U};
        // The page as it was compiled, to tell later whether the page has
        // moved on from it.
        Document document;
        Compiled compiled;
    };

    CompileWorker();
    ~CompileWorker();
    CompileWorker(const CompileWorker&) = delete;
    CompileWorker& operator=(const CompileWorker&) = delete;

    // Compiles `document`, which stands at `revision`, for the preview.
    void submit(const Document& document, std::uint64_t revision);
    // The newest compile not yet taken, if it is newer than everything taken
    // or fenced before; nothing otherwise.
    [[nodiscard]] std::optional<Result> take();
    // A compile of `revision` was made elsewhere and taken: nothing older is
    // handed out from now on.
    void fence(std::uint64_t revision);
    // Nothing waiting and nothing being compiled.
    [[nodiscard]] bool idle() const;
    // Blocks until idle. For tests and for closing.
    void wait_idle();

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable settled_;
    std::optional<std::pair<Document, std::uint64_t>> waiting_;
    std::optional<Result> ready_;
    // The newest revision handed out or fenced.
    std::uint64_t fence_{0U};
    bool busy_{false};
    bool stop_{false};
    std::thread thread_;
};

} // namespace ajfx::studio
