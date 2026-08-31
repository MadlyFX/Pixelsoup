#pragma once

#include <cstddef>
#include <optional>

#include <mediaserver/net/session_protocol.hpp>

namespace mediaserver::app {

struct OutputMediaTranscodeProgress final {
    core::u64 completed_frames{};
    core::u64 total_frames{};
    bool queued{};

    bool operator==(const OutputMediaTranscodeProgress&) const = default;
};

struct OutputMediaRuntimeSummary final {
    net::RuntimeMediaDecodeState decode{
        net::RuntimeMediaDecodeState::Unknown};
    net::RuntimeProxyState proxy{net::RuntimeProxyState::Source};
    net::RuntimeProxyReason proxy_reason{
        net::RuntimeProxyReason::SourcePolicy};
    net::RuntimeProxyIssue proxy_issue{net::RuntimeProxyIssue::None};
    std::optional<OutputMediaTranscodeProgress> transcode_progress;

    bool operator==(const OutputMediaRuntimeSummary&) const = default;
};

// Reduces one asset's reports from all configured output nodes. Missing nodes
// are explicit observations: a proxy cannot be called ready until every
// output reports it, while any reported software-only decoder is enough to
// warn the operator.
class OutputMediaRuntimeAccumulator final {
public:
    OutputMediaRuntimeAccumulator(
        std::size_t expected_nodes,
        net::RuntimeProxyState missing_proxy,
        net::RuntimeProxyReason missing_reason) noexcept
        : expected_nodes_(expected_nodes), missing_proxy_(missing_proxy),
          missing_reason_(missing_reason) {}

    void observe(const net::RuntimeMediaAssetStatus* status) noexcept {
        ++observed_slots_;
        if (status == nullptr) {
            all_nodes_reported_ = false;
            all_decode_known_ = false;
            all_hardware_ = false;
            return;
        }
        switch (status->decode) {
        case net::RuntimeMediaDecodeState::Unknown:
            all_decode_known_ = false;
            all_hardware_ = false;
            break;
        case net::RuntimeMediaDecodeState::HardwareAccelerated:
            break;
        case net::RuntimeMediaDecodeState::SoftwareOnly:
            any_software_ = true;
            all_hardware_ = false;
            break;
        case net::RuntimeMediaDecodeState::Unavailable:
            any_unavailable_ = true;
            all_hardware_ = false;
            break;
        }
        if (!worst_proxy_.has_value() ||
            proxy_priority(status->proxy) >
                proxy_priority(worst_proxy_->proxy)) {
            worst_proxy_ = *status;
        }
        if (status->has_transcode_progress &&
            (!progress_.has_value() || slower(*status, *progress_))) {
            progress_ = OutputMediaTranscodeProgress{
                status->transcode_completed_frames,
                status->transcode_total_frames,
                status->transcode_queued};
        }
    }

    [[nodiscard]] OutputMediaRuntimeSummary finish() const noexcept {
        OutputMediaRuntimeSummary result{};
        if (any_unavailable_) {
            result.decode = net::RuntimeMediaDecodeState::Unavailable;
        } else if (any_software_) {
            result.decode = net::RuntimeMediaDecodeState::SoftwareOnly;
        } else if (expected_nodes_ != 0U && all_decode_known_ &&
                   all_hardware_ && observed_slots_ == expected_nodes_) {
            result.decode =
                net::RuntimeMediaDecodeState::HardwareAccelerated;
        }

        result.proxy = worst_proxy_.has_value()
            ? worst_proxy_->proxy
            : missing_proxy_;
        result.proxy_reason = worst_proxy_.has_value()
            ? worst_proxy_->proxy_reason
            : missing_reason_;
        result.proxy_issue = worst_proxy_.has_value()
            ? worst_proxy_->proxy_issue
            : net::RuntimeProxyIssue::None;
        const bool missing = observed_slots_ != expected_nodes_ ||
            !all_nodes_reported_;
        if (missing && proxy_priority(missing_proxy_) >
                           proxy_priority(result.proxy)) {
            result.proxy = missing_proxy_;
            result.proxy_reason = missing_reason_;
            result.proxy_issue = net::RuntimeProxyIssue::None;
        }
        result.transcode_progress = progress_;
        return result;
    }

private:
    [[nodiscard]] static int proxy_priority(
        net::RuntimeProxyState state) noexcept {
        switch (state) {
        case net::RuntimeProxyState::Source:
            return 0;
        case net::RuntimeProxyState::ProxyReady:
            return 1;
        case net::RuntimeProxyState::PreferFallback:
            return 2;
        case net::RuntimeProxyState::RequiredUnready:
            return 3;
        case net::RuntimeProxyState::Failed:
            return 4;
        }
        return 4;
    }

    [[nodiscard]] static bool slower(
        const net::RuntimeMediaAssetStatus& candidate,
        const OutputMediaTranscodeProgress& current) noexcept {
        if (candidate.transcode_queued != current.queued) {
            return candidate.transcode_queued;
        }
        if (candidate.transcode_total_frames == 0U ||
            current.total_frames == 0U) {
            return candidate.transcode_total_frames == 0U &&
                current.total_frames != 0U;
        }
        const long double candidate_fraction =
            static_cast<long double>(candidate.transcode_completed_frames) /
            static_cast<long double>(candidate.transcode_total_frames);
        const long double current_fraction =
            static_cast<long double>(current.completed_frames) /
            static_cast<long double>(current.total_frames);
        return candidate_fraction < current_fraction;
    }

    std::size_t expected_nodes_{};
    std::size_t observed_slots_{};
    net::RuntimeProxyState missing_proxy_{net::RuntimeProxyState::Source};
    net::RuntimeProxyReason missing_reason_{
        net::RuntimeProxyReason::SourcePolicy};
    bool all_nodes_reported_{true};
    bool all_decode_known_{true};
    bool all_hardware_{true};
    bool any_software_{};
    bool any_unavailable_{};
    std::optional<net::RuntimeMediaAssetStatus> worst_proxy_;
    std::optional<OutputMediaTranscodeProgress> progress_;
};

} // namespace mediaserver::app
