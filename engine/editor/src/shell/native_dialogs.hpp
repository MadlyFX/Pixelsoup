#pragma once

// The native file dialog is intentionally the only platform-flavoured helper
// left in the editor. Its header is external code, so keep it outside the
// project's warnings-as-errors surface.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <tinyfiledialogs.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <mutex>

namespace mediaserver::editor {

// tinyfiledialogs answers in static buffers, so only one dialog may be open at
// a time. The frame thread only ever try-locks this: a picker asked for while
// another dialog is up is refused instead of stalling the frame.
[[nodiscard]] inline std::mutex &native_dialog_mutex() noexcept {
  static std::mutex mutex;
  return mutex;
}

} // namespace mediaserver::editor
