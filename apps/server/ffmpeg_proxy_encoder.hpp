#pragma once

#include "proxy_generation_service.hpp"

#include <memory>

namespace mediaserver::server {

enum class ProxyTranscodePriority : core::u8 {
    Background,
    Balanced,
    Fast,
};

// Production MP4 proxy backend for the registered H.264 and HEVC Main 10
// profiles. Decoding remains software-compatible for camera formats the local
// GPU cannot read; encoding prefers the matching local hardware encoder and
// falls back to FFmpeg's available software encoder.
[[nodiscard]] std::unique_ptr<ProxyEncoderBackend> ffmpeg_proxy_encoder_backend(
    ProxyTranscodePriority priority = ProxyTranscodePriority::Background);

} // namespace mediaserver::server
