#pragma once

#include "waveform_generation_service.hpp"

namespace mediaserver::server {

// Production PCM decoder for the waveform service. It opens the exact authored
// source, decodes the best audio stream to packed native-rate binary32 PCM,
// observes cancellation between packets/frames, and re-fingerprints the bytes
// it consumed before returning. No audio rendition or Asset identity is
// authored by this backend.
[[nodiscard]] WaveformDecoderFactory ffmpeg_waveform_decoder_factory();

} // namespace mediaserver::server
