#include "tracker_listener.hpp"

#include <mediaserver/track/freed.hpp>
#include <mediaserver/track/opentrackio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>

namespace mediaserver::server {
namespace {

// A FreeD D1 message is 29 bytes and an OpenTrackIO sample is a couple of
// kilobytes. This is the largest datagram the socket will read; anything
// longer is truncated by the receive and then refused by the parser, which is
// the correct outcome for a sender this is not talking to.
constexpr std::size_t max_datagram_bytes = track::max_opentrackio_sample_bytes;

} // namespace

std::string_view tracker_listener_state_name(
    TrackerListenerState state) noexcept {
    switch (state) {
    case TrackerListenerState::Idle:
        return "IDLE";
    case TrackerListenerState::BindFailed:
        return "BIND FAILED";
    case TrackerListenerState::Waiting:
        return "WAITING";
    case TrackerListenerState::Receiving:
        return "RECEIVING";
    }
    return "IDLE";
}

struct TrackerListener::State final {
    mutable std::mutex mutex;
    TrackerListenerStatus status;
    track::PoseSlot slot;
    std::atomic<core::u64> sample_after_publication{0U};

    asio::io_context context;
    std::optional<asio::ip::udp::socket> socket;
    std::thread worker;
    std::atomic_bool stop_requested{false};
    // Owned by the listener rather than the handler, because an asynchronous
    // receive writes into them after the call that started it has returned.
    std::vector<std::byte> buffer;
    asio::ip::udp::endpoint from;
    // The receiver's own datagram ordinal. FreeD carries no sequence number,
    // and a consumer still has to tell a repeated pose from a stalled sender.
    core::u64 received_ordinal{};

    void record(const track::TrackedPose& pose,
                scene::Turns field_of_view) {
        const std::lock_guard lock{mutex};
        ++status.accepted;
        status.last_pose = pose;
        status.field_of_view = field_of_view;
        status.state = TrackerListenerState::Receiving;
    }

    void refuse(track::TrackerParseError reason) {
        const std::lock_guard lock{mutex};
        switch (reason) {
        case track::TrackerParseError::Truncated:
            ++status.truncated;
            break;
        case track::TrackerParseError::UnrecognizedMessage:
            ++status.unrecognized;
            break;
        case track::TrackerParseError::ChecksumMismatch:
            ++status.checksum_failures;
            break;
        case track::TrackerParseError::InvalidValue:
            ++status.invalid_values;
            break;
        }
    }
};

TrackerListener::TrackerListener() : state_(std::make_unique<State>()) {}

TrackerListener::~TrackerListener() { stop(); }

bool TrackerListener::deliver(
    std::span<const std::byte> datagram) noexcept {
    scene::TrackingSourceSettings source{};
    core::u64 ordinal = 0U;
    {
        const std::lock_guard lock{state_->mutex};
        ++state_->status.datagrams;
        ++state_->received_ordinal;
        ordinal = state_->received_ordinal;
        source = state_->status.source;
    }

    track::TrackedPose pose{};
    float focal_length = 0.0F;
    // Whether the datagram said which camera it is for. FreeD always does;
    // OpenTrackIO's `sourceNumber` is optional.
    bool identifies_camera = true;
    if (source.protocol == scene::TrackerProtocolKind::FreeD) {
        const auto parsed = track::parse_freed_d1(datagram, ordinal);
        if (!parsed) {
            state_->refuse(parsed.error());
            return false;
        }
        pose = parsed.value().pose;
    } else {
        const auto parsed = track::parse_opentrackio(datagram, ordinal);
        if (!parsed) {
            state_->refuse(parsed.error());
            return false;
        }
        pose = parsed.value().pose;
        identifies_camera = parsed.value().identifies_camera;
        focal_length = pose.focal_length_mm;
    }

    // Two cameras can share a port, so a datagram for a different one is an
    // ordinary event rather than a fault. It is counted so an operator who has
    // set the wrong camera id can see why nothing is tracking.
    //
    // A sample that does not identify its camera is matched only by a feed
    // that does not ask for one. Authoring camera 3 and receiving datagrams
    // that name no camera is a genuine mismatch and is counted as such - but
    // it must not be confused with a sample that said "camera 0", which is
    // what happened while the OpenTrackIO parser never set the id at all and
    // every non-zero feed refused everything it received.
    if (!identifies_camera ? source.camera_id != 0U
                           : pose.camera_id != source.camera_id) {
        const std::lock_guard lock{state_->mutex};
        ++state_->status.foreign_camera;
        return false;
    }

    // The lens resolution happens here, on the Director, so every node
    // receives one already-decided angle rather than re-deriving it from a
    // profile each might hold a different version of.
    const scene::Turns field_of_view =
        scene::tracked_field_of_view(source, focal_length);
    if (!state_->slot.publish(pose)) {
        state_->refuse(track::TrackerParseError::InvalidValue);
        return false;
    }
    state_->record(pose, field_of_view);
    return true;
}

bool TrackerListener::configure(
    const scene::TrackingSourceSettings& source) noexcept {
    const scene::TrackingSourceSettings bounded = scene::clamped(source);
    {
        const std::lock_guard lock{state_->mutex};
        if (state_->status.source == bounded
            && state_->status.state != TrackerListenerState::Idle) {
            // Idempotent for an unchanged source: an operator adjusting the
            // sensor height should not drop the socket underneath a take.
            return state_->status.state != TrackerListenerState::BindFailed;
        }
    }
    stop();
    // A new source must not expose the previous socket's last pose while it
    // waits for its first packet. configure() has drained the only writer.
    state_->sample_after_publication.store(state_->slot.publications(), std::memory_order_release);
    {
        const std::lock_guard lock{state_->mutex};
        // A reconfigure starts the counters over, because they describe this
        // socket and this source rather than the process.
        state_->status = TrackerListenerStatus{};
        state_->status.source = bounded;
        state_->received_ordinal = 0U;
    }
    if (!bounded.enabled) {
        return true;
    }

    try {
        state_->context.restart();
        asio::ip::udp::socket socket{state_->context};
        std::error_code error;
        const asio::ip::udp::endpoint endpoint{
            asio::ip::udp::v4(), static_cast<unsigned short>(bounded.port)};
        socket.open(endpoint.protocol(), error);
        if (!error) {
            socket.set_option(asio::socket_base::reuse_address{true}, error);
        }
        if (!error) {
            socket.bind(endpoint, error);
        }
        if (error) {
            const std::lock_guard lock{state_->mutex};
            state_->status.state = TrackerListenerState::BindFailed;
            state_->status.last_error = error.message();
            return false;
        }
        socket.non_blocking(true, error);
        state_->socket.emplace(std::move(socket));
    } catch (const std::exception& failure) {
        const std::lock_guard lock{state_->mutex};
        state_->status.state = TrackerListenerState::BindFailed;
        state_->status.last_error = failure.what();
        return false;
    }

    {
        const std::lock_guard lock{state_->mutex};
        state_->status.state = TrackerListenerState::Waiting;
    }
    state_->stop_requested.store(false, std::memory_order_release);
    state_->buffer.assign(max_datagram_bytes, std::byte{});
    // Armed before the thread starts, so the context has work the moment it
    // runs and `run()` cannot return immediately on an idle tracker.
    arm_receive();
    try {
        // The thread does nothing but service completions. A datagram wakes it
        // when it arrives rather than at the next tick of a poll, which is the
        // whole point: the previous loop slept 100 ms whenever the socket was
        // empty, so at any real tracker rate it woke to find a burst of
        // datagrams queued, drained them back to back, and slept again. The
        // Director then sampled the same stale pose for most frames, and the
        // 0-100 ms of jitter that produced is not a fixed offset, so the
        // authored latency compensation could not correct it.
        state_->worker = std::thread{[this]() { state_->context.run(); }};
    } catch (const std::exception& failure) {
        const std::lock_guard lock{state_->mutex};
        state_->status.state = TrackerListenerState::BindFailed;
        state_->status.last_error = failure.what();
        state_->socket.reset();
        return false;
    }
    return true;
}

void TrackerListener::arm_receive() noexcept {
    if (!state_->socket
        || state_->stop_requested.load(std::memory_order_acquire)) {
        return;
    }
    state_->socket->async_receive_from(
        asio::buffer(state_->buffer.data(), state_->buffer.size()),
        state_->from,
        [this](const std::error_code& error, std::size_t received) {
            if (state_->stop_requested.load(std::memory_order_acquire)
                || error == asio::error::operation_aborted) {
                // Not re-armed, so the context runs out of work and the
                // worker returns even if nobody stopped it explicitly.
                return;
            }
            if (!error) {
                static_cast<void>(deliver(std::span<const std::byte>{
                    state_->buffer.data(), received}));
            } else if (error == asio::error::message_size) {
                // A datagram longer than the buffer. Windows reports this and
                // hands back the truncated bytes while Linux truncates
                // silently, so counting it here is what makes the two agree -
                // and an oversized datagram is a sender this feed is not
                // talking to, which the operator should be able to see.
                state_->refuse(track::TrackerParseError::Truncated);
            } else {
                const std::lock_guard lock{state_->mutex};
                state_->status.last_error = error.message();
            }
            arm_receive();
        });
}

void TrackerListener::stop() noexcept {
    state_->stop_requested.store(true, std::memory_order_release);
    // Bounded without a poll: this returns immediately, and `run()` returns as
    // soon as it observes it, so the join below waits for a thread that is
    // already on its way out rather than for a sleep to expire.
    state_->context.stop();
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
    if (state_->socket) {
        std::error_code ignored;
        state_->socket->close(ignored);
        state_->socket.reset();
    }
    const std::lock_guard lock{state_->mutex};
    if (state_->status.state != TrackerListenerState::BindFailed) {
        state_->status.state = TrackerListenerState::Idle;
    }
}

TrackerListenerStatus TrackerListener::status() const {
    const std::lock_guard lock{state_->mutex};
    return state_->status;
}

std::optional<track::PoseSample> TrackerListener::sample() const noexcept {
    const auto sample = state_->slot.sample();
    return sample && sample->publications > state_->sample_after_publication.load(std::memory_order_acquire)
        ? sample : std::nullopt;
}

TrackerListenerPool::TrackerListenerPool() = default;

TrackerListenerPool::~TrackerListenerPool() { stop(); }

const TrackerListenerPool::Entry *
TrackerListenerPool::find(doc::NodeId source) const noexcept {
    if (source.value() == 0U) {
        return nullptr;
    }
    for (const Entry &entry : entries_) {
        if (entry.node == source) {
            return &entry;
        }
    }
    return nullptr;
}

void TrackerListenerPool::configure(
    const std::vector<scene::TrackingSource> &sources) {
    // Drop what is no longer authored first, so a source that moved to another
    // node's port finds it free rather than in use by the listener it replaced.
    std::vector<Entry> retained;
    retained.reserve(entries_.size());
    for (Entry &entry : entries_) {
        const bool still_authored = std::any_of(
            sources.begin(), sources.end(),
            [&entry](const scene::TrackingSource &source) {
                return source.node == entry.node;
            });
        if (still_authored) {
            retained.push_back(std::move(entry));
        } else if (entry.listener) {
            entry.listener->stop();
        }
    }
    entries_ = std::move(retained);

    for (const scene::TrackingSource &source : sources) {
        if (entries_.size() >= scene::max_tracking_sources) {
            break;
        }
        auto found = std::find_if(
            entries_.begin(), entries_.end(),
            [&source](const Entry &entry) { return entry.node == source.node; });
        if (found == entries_.end()) {
            Entry entry{};
            entry.node = source.node;
            entry.name = source.name;
            try {
                entry.listener = std::make_unique<TrackerListener>();
            } catch (const std::bad_alloc &) {
                // A source that could not be opened simply is not listening,
                // which the panel already reports. Refusing the whole show
                // because one tracker could not allocate would be worse.
                continue;
            }
            entries_.push_back(std::move(entry));
            found = std::prev(entries_.end());
        }
        found->name = source.name;
        // Idempotent for an unchanged source, so an operator renaming a
        // tracker does not drop its socket.
        static_cast<void>(found->listener->configure(source.settings));
    }
}

void TrackerListenerPool::stop() noexcept {
    for (Entry &entry : entries_) {
        if (entry.listener) {
            entry.listener->stop();
        }
    }
}

std::optional<track::PoseSample>
TrackerListenerPool::sample(doc::NodeId source) const noexcept {
    const Entry *const entry = find(source);
    return entry != nullptr && entry->listener ? entry->listener->sample()
                                               : std::nullopt;
}

TrackerListenerStatus
TrackerListenerPool::status(doc::NodeId source) const noexcept {
    const Entry *const entry = find(source);
    if (entry == nullptr || !entry->listener) {
        return TrackerListenerStatus{};
    }
    return entry->listener->status();
}

scene::Turns
TrackerListenerPool::field_of_view(doc::NodeId source) const noexcept {
    return status(source).field_of_view;
}

float TrackerListenerPool::aspect_ratio(doc::NodeId source) const noexcept {
    const Entry *const entry = find(source);
    if (entry == nullptr || !entry->listener) {
        return 1.0F;
    }
    return entry->listener->status().source.sensor_aspect_ratio;
}

std::size_t TrackerListenerPool::open_sources() const noexcept {
    std::size_t open = 0U;
    for (const Entry &entry : entries_) {
        if (entry.listener && entry.listener->status().listening()) {
            ++open;
        }
    }
    return open;
}

} // namespace mediaserver::server
