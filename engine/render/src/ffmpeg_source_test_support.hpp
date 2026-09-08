#pragma once

#include <functional>

namespace mediaserver::render::detail {

// Deterministic CPU-test seam. The callback runs while the process video
// registry mutex is held, allowing tests to prove render-facing access drops
// instead of waiting. Production code must never call this function.
void hold_video_registry_lock_for_test(
    const std::function<void()>& callback);

} // namespace mediaserver::render::detail
