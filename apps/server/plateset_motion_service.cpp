#include "plateset_motion_service.hpp"

#include <mediaserver/assets/plateset.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace mediaserver::server {
namespace {

// The analysis frame: small enough to decode a take in minutes, large
// enough that a bump of a few source lines still moves a whole pixel.
constexpr int analysis_width = 160;
constexpr int analysis_height = 90;
constexpr std::array<char, 8U> cache_magic{'M', 'S', 'M', 'O', 'T', '0', '0', '1'};

struct FormatClose final {
  void operator()(AVFormatContext *context) const noexcept {
    avformat_close_input(&context);
  }
};
struct CodecFree final {
  void operator()(AVCodecContext *context) const noexcept {
    avcodec_free_context(&context);
  }
};
struct FrameFree final {
  void operator()(AVFrame *frame) const noexcept { av_frame_free(&frame); }
};
struct PacketFree final {
  void operator()(AVPacket *packet) const noexcept { av_packet_free(&packet); }
};
struct ScaleFree final {
  void operator()(SwsContext *context) const noexcept {
    sws_freeContext(context);
  }
};

// The shift, in whole and fractional pixels, that best maps `previous`
// onto `current`: current[i] ~ previous[i - shift].
// A profile with its mean and contrast taken out, so that driving into
// shade - every value scaled down - does not read as movement.
[[nodiscard]] std::vector<float> standardized(std::vector<float> values) {
  double mean = 0.0;
  for (const float value : values) {
    mean += value;
  }
  mean /= static_cast<double>(values.size());
  double spread = 0.0;
  for (const float value : values) {
    spread += (value - mean) * (value - mean);
  }
  spread = std::sqrt(spread / static_cast<double>(values.size()));
  for (float &value : values) {
    value = spread > 1.0e-6 ? static_cast<float>((value - mean) / spread)
                            : 0.0F;
  }
  return values;
}

[[nodiscard]] float profile_shift(const std::vector<float> &previous_raw,
                                  const std::vector<float> &current_raw,
                                  int reach) {
  const std::vector<float> previous = standardized(previous_raw);
  const std::vector<float> current = standardized(current_raw);
  const int size = static_cast<int>(current.size());
  std::vector<double> costs;
  costs.reserve(static_cast<std::size_t>(2 * reach + 1));
  for (int shift = -reach; shift <= reach; ++shift) {
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < size; ++i) {
      const int j = i - shift;
      if (j < 0 || j >= size) {
        continue;
      }
      sum += std::fabs(current[static_cast<std::size_t>(i)] -
                       previous[static_cast<std::size_t>(j)]);
      ++count;
    }
    costs.push_back(count > 0 ? sum / count : 1.0e9);
  }
  const auto best = static_cast<int>(
      std::min_element(costs.begin(), costs.end()) - costs.begin());
  float refined = static_cast<float>(best - reach);
  if (best > 0 && best + 1 < static_cast<int>(costs.size())) {
    const double left = costs[static_cast<std::size_t>(best - 1)];
    const double centre = costs[static_cast<std::size_t>(best)];
    const double right = costs[static_cast<std::size_t>(best + 1)];
    const double curve = left - 2.0 * centre + right;
    if (curve > 1.0e-9) {
      refined += static_cast<float>(0.5 * (left - right) / curve);
    }
  }
  return refined;
}

[[nodiscard]] std::string cache_name(const PlateSource &plate) {
  // FNV-1a over the content identity; the source path keeps two files that
  // happen to share a fingerprint apart.
  core::u64 hash = 1469598103934665603ULL;
  const auto feed = [&hash](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0U; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ULL;
    }
  };
  feed(plate.source.data(), plate.source.size());
  feed(&plate.fingerprint->byte_length, sizeof(plate.fingerprint->byte_length));
  feed(plate.fingerprint->sha256.bytes.data(),
       plate.fingerprint->sha256.bytes.size());
  std::array<char, 32U> text{};
  std::snprintf(text.data(), text.size(), "%016llx.msmotion",
                static_cast<unsigned long long>(hash));
  return text.data();
}

[[nodiscard]] std::optional<app::MotionSignature>
read_cached(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::array<char, 8U> magic{};
  core::u32 count = 0U;
  app::MotionSignature signature;
  in.read(magic.data(), magic.size());
  in.read(reinterpret_cast<char *>(&count), sizeof(count));
  in.read(reinterpret_cast<char *>(&signature.rate.numerator),
          sizeof(signature.rate.numerator));
  in.read(reinterpret_cast<char *>(&signature.rate.denominator),
          sizeof(signature.rate.denominator));
  if (!in || magic != cache_magic || count > 10'000'000U) {
    return std::nullopt;
  }
  for (std::vector<float> *signal :
       {&signature.dx, &signature.dy, &signature.luma, &signature.change}) {
    signal->resize(count);
    in.read(reinterpret_cast<char *>(signal->data()),
            static_cast<std::streamsize>(count * sizeof(float)));
  }
  return in ? std::optional<app::MotionSignature>{std::move(signature)}
            : std::nullopt;
}

void write_cached(const std::filesystem::path &path,
                  const app::MotionSignature &signature) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  const std::filesystem::path partial = path.string() + ".partial";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
      return;
    }
    const auto count = static_cast<core::u32>(signature.size());
    out.write(cache_magic.data(), cache_magic.size());
    out.write(reinterpret_cast<const char *>(&count), sizeof(count));
    out.write(reinterpret_cast<const char *>(&signature.rate.numerator),
              sizeof(signature.rate.numerator));
    out.write(reinterpret_cast<const char *>(&signature.rate.denominator),
              sizeof(signature.rate.denominator));
    for (const std::vector<float> *signal :
         {&signature.dx, &signature.dy, &signature.luma, &signature.change}) {
      out.write(reinterpret_cast<const char *>(signal->data()),
                static_cast<std::streamsize>(count * sizeof(float)));
    }
    if (!out) {
      return;
    }
  }
  std::filesystem::rename(partial, path, error);
}

[[nodiscard]] editor::PlatesetMotionEvent
to_editor(const app::MotionEvent &event) {
  return editor::PlatesetMotionEvent{event.frame, event.kind};
}

} // namespace

std::optional<app::MotionSignature>
decode_motion_signature(const PlateSource &plate, std::stop_token stop,
                        const std::function<void(core::u32)> &progress) {
  AVFormatContext *opened = nullptr;
  if (avformat_open_input(&opened, plate.source.c_str(), nullptr, nullptr) <
      0) {
    return std::nullopt;
  }
  std::unique_ptr<AVFormatContext, FormatClose> format{opened};
  if (avformat_find_stream_info(format.get(), nullptr) < 0) {
    return std::nullopt;
  }
  const int stream_index = av_find_best_stream(
      format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    return std::nullopt;
  }
  AVStream *const stream = format->streams[stream_index];
  const AVCodec *const codec =
      avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    return std::nullopt;
  }
  std::unique_ptr<AVCodecContext, CodecFree> context{
      avcodec_alloc_context3(codec)};
  if (!context ||
      avcodec_parameters_to_context(context.get(), stream->codecpar) < 0) {
    return std::nullopt;
  }
  // Every frame is needed, but not its finest detail: frame threads for
  // throughput and no loop filter, which only smooths block edges. A
  // quarter of the cores at most, so the editor and the show keep theirs.
  context->thread_count = static_cast<int>(
      std::max(1U, std::thread::hardware_concurrency() / 4U));
  context->thread_type = FF_THREAD_FRAME;
  context->skip_loop_filter = AVDISCARD_ALL;
  if (avcodec_open2(context.get(), codec, nullptr) < 0) {
    return std::nullopt;
  }
  for (unsigned index = 0U; index < format->nb_streams; ++index) {
    if (static_cast<int>(index) != stream_index) {
      format->streams[index]->discard = AVDISCARD_ALL;
    }
  }
  std::unique_ptr<AVPacket, PacketFree> packet{av_packet_alloc()};
  std::unique_ptr<AVFrame, FrameFree> frame{av_frame_alloc()};
  if (!packet || !frame) {
    return std::nullopt;
  }

  app::MotionSignature signature;
  signature.rate = plate.rate;
  if (plate.frame_count > 0U) {
    for (std::vector<float> *signal : {&signature.dx, &signature.dy,
                                       &signature.luma, &signature.change}) {
      signal->reserve(plate.frame_count);
    }
  }
  std::unique_ptr<SwsContext, ScaleFree> scale;
  std::vector<std::uint8_t> previous(
      static_cast<std::size_t>(analysis_width * analysis_height));
  std::vector<std::uint8_t> current(previous.size());
  std::vector<float> previous_rows(analysis_height);
  std::vector<float> previous_columns(analysis_width);
  std::vector<float> rows(analysis_height);
  std::vector<float> columns(analysis_width);

  const auto take = [&](const AVFrame &decoded) {
    if (!scale) {
      scale.reset(sws_getContext(
          decoded.width, decoded.height,
          static_cast<AVPixelFormat>(decoded.format), analysis_width,
          analysis_height, AV_PIX_FMT_GRAY8, SWS_AREA, nullptr, nullptr,
          nullptr));
      if (!scale) {
        return false;
      }
    }
    std::uint8_t *planes[1]{current.data()};
    const int strides[1]{analysis_width};
    sws_scale(scale.get(), decoded.data, decoded.linesize, 0, decoded.height,
              planes, strides);
    double total = 0.0;
    double difference = 0.0;
    std::fill(rows.begin(), rows.end(), 0.0F);
    std::fill(columns.begin(), columns.end(), 0.0F);
    for (int y = 0; y < analysis_height; ++y) {
      for (int x = 0; x < analysis_width; ++x) {
        const auto index = static_cast<std::size_t>(y * analysis_width + x);
        const float value = current[index] / 255.0F;
        total += value;
        difference += std::fabs(value - previous[index] / 255.0F);
        rows[static_cast<std::size_t>(y)] += value / analysis_width;
        columns[static_cast<std::size_t>(x)] += value / analysis_height;
      }
    }
    const bool first = signature.size() == 0U;
    signature.luma.push_back(
        static_cast<float>(total / (analysis_width * analysis_height)));
    signature.change.push_back(
        first ? 0.0F
              : static_cast<float>(difference /
                                   (analysis_width * analysis_height)));
    signature.dy.push_back(first ? 0.0F
                                 : profile_shift(previous_rows, rows, 8));
    signature.dx.push_back(first ? 0.0F
                                 : profile_shift(previous_columns, columns, 12));
    previous.swap(current);
    previous_rows.swap(rows);
    previous_columns.swap(columns);
    if (progress && signature.size() % 24U == 0U) {
      progress(static_cast<core::u32>(signature.size()));
    }
    return true;
  };

  const auto drain = [&]() {
    while (avcodec_receive_frame(context.get(), frame.get()) == 0) {
      const bool ok = take(*frame);
      av_frame_unref(frame.get());
      if (!ok) {
        return false;
      }
    }
    return true;
  };
  while (av_read_frame(format.get(), packet.get()) >= 0) {
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    if (packet->stream_index == stream_index) {
      if (avcodec_send_packet(context.get(), packet.get()) < 0) {
        av_packet_unref(packet.get());
        break;
      }
      if (!drain()) {
        return std::nullopt;
      }
    }
    av_packet_unref(packet.get());
  }
  avcodec_send_packet(context.get(), nullptr);
  if (!drain() || signature.size() < 2U) {
    return std::nullopt;
  }
  return signature;
}

PlatesetMotionService::PlatesetMotionService(
    std::filesystem::path cache_directory)
    : cache_directory_(std::move(cache_directory)),
      worker_([this](std::stop_token stop) { run(stop); }) {}

PlatesetMotionService::~PlatesetMotionService() {
  worker_.request_stop();
  wake_.notify_all();
}

void PlatesetMotionService::analyse(doc::NodeId plateset, PlateSources plates,
                                    Pathway pathway) {
  {
    std::lock_guard lock{mutex_};
    std::erase_if(queue_, [plateset](const Job &job) {
      return job.plateset == plateset;
    });
    constexpr std::size_t most_waiting = 4U;
    while (queue_.size() >= most_waiting) {
      views_.erase(queue_.front().plateset);
      queue_.pop_front();
    }
    // Only the front plate's moments are wanted: the other plates are
    // not decoded at all.
    if (pathway == Pathway::Moments) {
      std::optional<std::size_t> keep;
      for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
        if (plates[slot].has_value()) {
          keep = slot;
          break;
        }
      }
      for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
        if (slot != keep) {
          plates[slot].reset();
        }
      }
    }
    queue_.push_back(Job{plateset, std::move(plates), pathway});
    editor::PlatesetAnalysisView queued = views_[plateset];
    queued.state = editor::PlatesetAnalysisState::Queued;
    queued.progress = 0.0F;
    queued.detail = "Waiting to analyse";
    views_[plateset] = std::move(queued);
  }
  wake_.notify_one();
}

editor::PlatesetAnalysisView
PlatesetMotionService::view(doc::NodeId plateset) const {
  std::lock_guard lock{mutex_};
  const auto found = views_.find(plateset);
  return found != views_.end() ? found->second
                               : editor::PlatesetAnalysisView{};
}

void PlatesetMotionService::publish(doc::NodeId plateset,
                                    editor::PlatesetAnalysisView view) {
  std::lock_guard lock{mutex_};
  views_[plateset] = std::move(view);
}

std::optional<app::MotionSignature>
PlatesetMotionService::signature(const PlateSource &plate,
                                 std::stop_token stop,
                                 const std::function<void(core::u32)> &progress) {
  std::string key = plate.source;
  if (plate.fingerprint.has_value()) {
    key += '|' + std::to_string(plate.fingerprint->byte_length);
  }
  if (const auto found = memory_.find(key); found != memory_.end()) {
    return found->second;
  }
  std::optional<std::filesystem::path> cached;
  if (plate.fingerprint.has_value() && !cache_directory_.empty()) {
    cached = cache_directory_ / cache_name(plate);
    if (auto read = read_cached(*cached)) {
      memory_.emplace(key, *read);
      return read;
    }
  }
  std::optional<app::MotionSignature> decoded =
      decode_motion_signature(plate, stop, progress);
  if (decoded.has_value()) {
    if (cached.has_value()) {
      write_cached(*cached, *decoded);
    }
    memory_.emplace(key, *decoded);
  }
  return decoded;
}

void PlatesetMotionService::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Job job;
    {
      std::unique_lock lock{mutex_};
      wake_.wait(lock, stop, [this] { return !queue_.empty(); });
      if (stop.stop_requested()) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    core::u64 total = 0U;
    for (const std::optional<PlateSource> &plate : job.plates) {
      if (plate.has_value()) {
        total += std::max<core::u32>(plate->frame_count, 1U);
      }
    }
    core::u64 done = 0U;
    std::array<std::optional<app::MotionSignature>,
               assets::plateset_slot_count>
        signatures{};
    // A Moments request keeps what an earlier sync found.
    editor::PlatesetAnalysisView running = view(job.plateset);
    if (job.pathway == Pathway::Sync) {
      running = editor::PlatesetAnalysisView{};
    }
    running.state = editor::PlatesetAnalysisState::Running;
    bool failed = false;
    for (std::size_t slot = 0U; slot < job.plates.size(); ++slot) {
      if (!job.plates[slot].has_value()) {
        continue;
      }
      const PlateSource &plate = *job.plates[slot];
      running.detail =
          "Reading " + std::string{assets::plateset_slot_label(
                           static_cast<assets::PlatesetSlot>(slot))};
      publish(job.plateset, running);
      signatures[slot] = signature(
          plate, stop, [&](core::u32 frames) {
            running.progress = total == 0U
                                   ? 0.0F
                                   : static_cast<float>(done + frames) /
                                         static_cast<float>(total);
            publish(job.plateset, running);
          });
      if (stop.stop_requested()) {
        return;
      }
      if (!signatures[slot].has_value()) {
        failed = true;
        running.detail =
            "Could not read " + std::string{assets::plateset_slot_label(
                                    static_cast<assets::PlatesetSlot>(slot))};
      }
      done += std::max<core::u32>(plate.frame_count, 1U);
    }

    editor::PlatesetAnalysisView result = running;
    result.events.clear();
    if (job.pathway == Pathway::Sync) {
      const app::MotionSync sync = app::estimate_plateset_sync(signatures);
      result.offsets = sync.offsets;
      result.confidence = sync.confidence;
      result.drift = sync.drift;
      result.synced = true;
    }
    const std::optional<app::MotionSignature> *front = nullptr;
    for (const std::optional<app::MotionSignature> &plate : signatures) {
      if (plate.has_value()) {
        front = std::addressof(plate);
        break;
      }
    }
    if (front != nullptr) {
      for (const app::MotionEvent &event : app::detect_motion_events(**front)) {
        result.events.push_back(to_editor(event));
      }
    }
    result.moments_found = front != nullptr;
    const bool any = std::any_of(signatures.begin(), signatures.end(),
                                 [](const auto &plate) {
                                   return plate.has_value();
                                 });
    result.state = any ? editor::PlatesetAnalysisState::Done
                       : editor::PlatesetAnalysisState::Failed;
    result.progress = 1.0F;
    result.detail = failed ? running.detail
                    : any  ? std::string{"Analysed"}
                           : std::string{"No plate could be read"};
    publish(job.plateset, std::move(result));
  }
}

} // namespace mediaserver::server
