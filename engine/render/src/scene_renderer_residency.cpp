// SceneRenderer residency: streaming geometry and model textures within a
// recording's budget, and adopting models as they become resident.

#include "scene_renderer_internal.hpp"

namespace mediaserver::render {

using namespace scene_detail;

core::u64 SceneRenderer::RecordingBudget::remaining(
    rhi::CommandList& commands, core::u64 limit) {
    const auto current = commands.recording_token();
    if (!current) {
        return 0U;
    }
    if (!recording || !recording.same_recording(current)) {
        recording = current;
        used = 0U;
    }
    return used >= limit ? 0U : limit - used;
}

bool SceneRenderer::stream_geometry(
    GeometryResidency::Entry& entry,
    rhi::CommandList& commands,
    bool complete_now) {
    const auto budget = complete_now
        ? std::numeric_limits<core::u64>::max()
        : geometry_budget_.remaining(commands, limits_.geometry_upload_bytes);
    const auto streamed = geometry_->stream(entry, commands, complete_now, budget);
    if (streamed.written != 0U) {
        stats_.geometry_uploaded = true;
        stats_.upload_bytes += streamed.written;
        if (!complete_now) {
            geometry_budget_.used += streamed.written;
        }
    }
    return streamed.drawable;
}

SceneRenderer::TextureUpload SceneRenderer::stream_model_textures(
    PendingModelIntegration& integration, rhi::CommandList& commands) {
    using rhi::CommandRecordingOutcome;
    // Upload space left for the draw data recorded after these rows.
    constexpr core::u64 upload_reserve = 16ULL << 20U;
    const auto position = [](core::u64 texture, core::u64 mip, core::u64 row) noexcept {
        return texture << 40U | mip << 32U | row;
    };
    const auto current = commands.recording_token();
    auto& writes = integration.texture_writes;
    auto& next = integration.texture_next;
    // A canceled recording never ran: upload again from its first row.
    for (auto write = writes.begin(); write != writes.end(); ++write) {
        const auto outcome = write->recording.outcome();
        if (outcome == CommandRecordingOutcome::Canceled
            || outcome == CommandRecordingOutcome::Unavailable) {
            next = std::min(next, write->begin);
            writes.erase(write, writes.end());
            break;
        }
    }
    while (!writes.empty()
           && writes.front().recording.outcome() == CommandRecordingOutcome::Submitted) {
        writes.erase(writes.begin());
    }

    auto& targets = integration.candidate.textures;
    const auto& sources = integration.decoded_asset->textures;
    const core::u64 end = position(targets.size(), 0U, 0U);
    core::u64 budget = current ? texture_budget_.remaining(commands, limits_.texture_upload_bytes) : 0U;
    while (next < end) {
        const auto index = static_cast<std::size_t>(next >> 40U);
        const auto mip = static_cast<core::u32>((next >> 32U) & 0xFFU);
        const auto row = static_cast<core::u32>(next & 0xFFFF'FFFFULL);
        if (index >= sources.size() || !sources[index].valid() || !targets[index]) {
            next = position(index + 1U, 0U, 0U);
            continue;
        }
        const ModelTexture& source = sources[index];
        const auto width = mip == 0U ? source.width : source.lower_mips[mip - 1U].width;
        const auto height = mip == 0U ? source.height : source.lower_mips[mip - 1U].height;
        const std::span<const core::u8> pixels =
            mip == 0U ? source.rgba : source.lower_mips[mip - 1U].rgba;
        const core::u32 block = source.encoding == ModelTextureEncoding::Rgba8 ? 1U : 4U;
        const core::u64 row_bytes = block == 1U ? static_cast<core::u64>(width) * 4U
            : static_cast<core::u64>((width + 3U) / 4U) * 16U;
        // The list's capacity is measured in rows of the widest level, so
        // it holds at least as many rows of a narrower one.
        const core::u32 capacity =
            commands.texture_upload_row_capacity(targets[index], upload_reserve);
        // A row wider than the whole limit still goes, alone, so any limit
        // makes progress.
        const core::u64 budget_rows = texture_budget_.used == 0U
            ? std::max<core::u64>(budget / row_bytes, 1U) : budget / row_bytes;
        const auto block_rows = static_cast<core::u32>(std::min<core::u64>({
            (height - row + block - 1U) / block,
            (static_cast<core::u64>(capacity) + block - 1U) / block,
            budget_rows}));
        if (!current || block_rows == 0U) {
            break;
        }
        const auto rows = std::min(height - row, block_rows * block);
        const auto bytes = pixels.subspan(
            static_cast<std::size_t>((row / block) * row_bytes),
            static_cast<std::size_t>(block_rows * row_bytes));
        if (!commands.upload_texture_rows(targets[index], mip, 0U, row, rows, bytes)) {
            return TextureUpload::Failed;
        }
        budget -= std::min<core::u64>(budget, bytes.size());
        texture_budget_.used += bytes.size();
        stats_.upload_bytes += bytes.size();
        if (writes.empty() || !writes.back().recording.same_recording(current)) {
            writes.push_back({current, next});
        }
        next = row + rows < height ? position(index, mip, row + rows)
            : mip < source.lower_mips.size() ? position(index, mip + 1U, 0U)
            : position(index + 1U, 0U, 0U);
    }
    const bool landed = next >= end && std::all_of(writes.begin(), writes.end(),
        [&](const PendingModelIntegration::TextureWrite& write) noexcept {
            return current && current.same_recording(write.recording);
        });
    return landed ? TextureUpload::Drawable : TextureUpload::Pending;
}

bool SceneRenderer::streaming_geometry(const std::string& source) const noexcept {
    const auto found = resident_models_.find(source);
    return found != resident_models_.end()
        && std::any_of(pending_model_integrations_.begin(), pending_model_integrations_.end(),
            [&](const PendingModelIntegration& integration) noexcept {
                return integration.active == &found->second && !integration.recording
                    && !integration.blocked;
            });
}

void SceneRenderer::note_use(const ResidentModel& model, rhi::CommandList& commands) {
    const auto current = commands.recording_token();
    if (!current) {
        // Without a token this use cannot be followed; such a model is never
        // reclaimed rather than reclaimed early.
        model.untracked_use = true;
        return;
    }
    std::erase_if(model.uses, [](const rhi::CommandRecordingToken& token) {
        return token.outcome() != rhi::CommandRecordingOutcome::Pending;
    });
    // The geometry may be shared with renderers that outlive this one, so
    // its own use list decides when its slots are reused.
    if (model.geometry) {
        geometry_->note_use(*model.geometry, commands);
    }
    for (const auto& token : model.uses) {
        if (token.same_recording(current)) {
            return;
        }
    }
    model.uses.push_back(current);
}

void SceneRenderer::reclaim_retired_models() noexcept {
    // Bounded per recording: released textures are destroyed here.
    std::size_t released{};
    for (auto model = retired_models_.begin();
         model != retired_models_.end() && released < 8U;) {
        const bool in_use = model->untracked_use
            || std::any_of(model->uses.begin(), model->uses.end(),
                [](const rhi::CommandRecordingToken& token) {
                    return token.outcome() == rhi::CommandRecordingOutcome::Pending;
                });
        const bool textures_busy = std::any_of(
            model->textures.begin(), model->textures.end(),
            [](const rhi::Texture& texture) { return !texture.safe_to_release(); });
        if (in_use || textures_busy) {
            ++model;
            continue;
        }
        // Erasing drops this renderer's hold on the geometry; the residency
        // reuses the slots once no renderer holds them.
        model_texture_bytes_ -= std::min(model_texture_bytes_, model->texture_bytes);
        model = retired_models_.erase(model);
        ++released;
    }
    if (released != 0U) {
        stats_.model_texture_bytes = texture_streamer_
            ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    }
    geometry_->collect();
}

const SceneRenderer::MeshRange* SceneRenderer::ensure_resident(
    scene::MeshKey key, rhi::CommandList& commands) {
    const auto normal_key = scene::normalized(key);
    auto found = resident_.find(normal_key);
    if (found == resident_.end()) {
        bool grew = false;
        auto* const entry = geometry_->procedural(normal_key, commands, grew);
        stats_.geometry_uploaded = stats_.geometry_uploaded || grew;
        if (entry == nullptr) {
            return nullptr;
        }
        try {
            found = resident_.emplace(normal_key, ResidentMesh{
                MeshRange{entry->span.first_index, entry->span.index_count}, entry}).first;
        } catch (...) {
            return nullptr;
        }
    }
    // Procedural meshes are drawn in the recording that needs them, so their
    // (small) upload is all made here or the instance waits.
    if (!stream_geometry(*found->second.entry, commands, true)) {
        return nullptr;
    }
    return &found->second.range;
}

bool SceneRenderer::resolve_recording_transactions(
    rhi::CommandList& commands) {
    reclaim_retired_models();
    rhi::CommandRecordingToken current_recording;
    const auto is_current = [&](const rhi::CommandRecordingToken& pending) {
        if (!current_recording) {
            current_recording = commands.recording_token();
        }
        return current_recording
            && current_recording.same_recording(pending);
    };

    for (auto pending = pending_model_integrations_.begin();
         pending != pending_model_integrations_.end();) {
        if (!pending->recording) {
            ++pending;
            continue;
        }
        const auto outcome = pending->recording.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Pending) {
            if (!is_current(pending->recording)) {
                // A second outer recording cannot sample provisional textures
                // while the first recording still owns their only uploads.
                return false;
            }
            ++pending;
            continue;
        }
        if (outcome == rhi::CommandRecordingOutcome::Submitted) {
            if (pending->active == nullptr) {
                return false;
            }
            if (pending->active->generation != 0U) {
                if (retired_models_.size()
                    >= limits_.max_model_refresh_generations) {
                    return false;
                }
                retired_models_.push_back(std::move(*pending->active));
            }
            *pending->active = std::move(pending->candidate);
            pending = pending_model_integrations_.erase(pending);
            continue;
        }

        // Canceled (or conservatively unavailable) candidate resources remain
        // state-owned and reusable. Their texture copies are re-recorded into
        // the next command list, while their already-appended mesh ranges and
        // driver allocations consume no additional retirement/arena capacity.
        // The authored active generation was never overwritten.
        if (pending->active == nullptr) {
            return false;
        }
        pending->recording = {};
        pending->blocked = false;
        ++pending;
    }

    const auto reconcile_upload = [&](rhi::CommandRecordingToken& token,
                                      bool& state,
                                      bool canceled_state) {
        if (!token) {
            return true;
        }
        switch (token.outcome()) {
        case rhi::CommandRecordingOutcome::Submitted:
            token = {};
            return true;
        case rhi::CommandRecordingOutcome::Canceled:
        case rhi::CommandRecordingOutcome::Unavailable:
            state = canceled_state;
            token = {};
            return true;
        case rhi::CommandRecordingOutcome::Pending:
            return is_current(token);
        }
        return false;
    };
    return reconcile_upload(
               material_fallback_upload_recording_,
               material_fallback_uploaded_, false);
}

const SceneRenderer::ResidentModel* SceneRenderer::ensure_model_resident(
    const std::string& source,
    rhi::CommandList& commands,
    core::u64 publication_fence) {
    // Whatever this returns is drawn in this recording: note it, so a later
    // retirement reuses its slots only after the recording is submitted.
    const ResidentModel* const model = integrate_model(source, commands, publication_fence);
    if (model != nullptr) {
        note_use(*model, commands);
    }
    return model;
}

const SceneRenderer::ResidentModel* SceneRenderer::integrate_model(
    const std::string& source,
    rhi::CommandList& commands,
    core::u64 publication_fence) {
    auto found = resident_models_.find(source);
    const bool has_resident = found != resident_models_.end()
        && found->second.generation != 0U;
    auto pending = found == resident_models_.end()
        ? pending_model_integrations_.end()
        : std::find_if(
            pending_model_integrations_.begin(),
            pending_model_integrations_.end(),
            [&](const PendingModelIntegration& integration) noexcept {
                return integration.active == &found->second;
            });
    if (pending != pending_model_integrations_.end()
        && pending->recording.outcome()
            == rhi::CommandRecordingOutcome::Pending) {
        const auto current = commands.recording_token();
        if (current && current.same_recording(pending->recording)) {
            return &pending->candidate;
        }
        return has_resident ? &found->second : nullptr;
    }

    const ModelCacheObservation observed =
        model_cache_.observe(source, publication_fence, authored_asset_generation_);
    switch (observed.state) {
    case ModelCacheState::Pending:
    case ModelCacheState::Busy:
        ++stats_.models_pending;
        break;
    case ModelCacheState::Failed:
        ++stats_.models_failed;
        break;
    case ModelCacheState::Ready:
    case ModelCacheState::Unseen:
        break;
    }
    if (observed.stale_ready) {
        ++stats_.models_stale;
    }

    // A candidate leaving its transaction may still have writes in flight
    // into its slots and textures: it is retired, and they are reused only
    // after those recordings.
    const auto retire_candidate = [&](PendingModelIntegration& integration) {
        if (integration.candidate.geometry) {
            for (const auto& write : integration.candidate.geometry->writes) {
                integration.candidate.uses.push_back(write.recording);
            }
        }
        for (const auto& write : integration.texture_writes) {
            integration.candidate.uses.push_back(write.recording);
        }
        retired_models_.push_back(std::move(integration.candidate));
    };

    // A canceled-generation retry is superseded only after the command's
    // publication fence can see a different complete asset. A newer live
    // request may still be Pending or Failed while `observed` intentionally
    // returns this candidate's stale generation; discarding it merely because
    // current_generation advanced would make one recording depend on worker
    // timing outside its frozen epoch.
    if (pending != pending_model_integrations_.end()
        && observed.ready()
        && observed.generation != 0U
        && observed.generation != pending->generation) {
        // Explicit refresh supersedes the canceled candidate. Keep its driver
        // resources in the already-reserved retirement slot rather than
        // destroying them on the frame thread.
        retire_candidate(*pending);
        pending = pending_model_integrations_.erase(pending);
    }

    const bool retry = pending != pending_model_integrations_.end()
        && !pending->recording;
    const std::shared_ptr<const ModelAsset> decoded_asset = retry
        ? pending->decoded_asset
        : observed.asset;
    const core::u64 decoded_generation = retry
        ? pending->generation
        : observed.generation;

    // A contended/pending/failed cache never displaces complete GPU geometry.
    // Likewise, a command-list fence may deliberately hide a CPU publication
    // that completed after another mapped view began this recording.
    if (decoded_asset == nullptr || (!retry && !observed.ready())) {
        return has_resident ? &found->second : nullptr;
    }
    if (has_resident && found->second.generation == decoded_generation) {
        return &found->second;
    }
    if (texture_streamer_ && !texture_streamer_->contains(source, decoded_generation, *decoded_asset)) {
        ++stats_.models_pending;
        return has_resident ? &found->second : nullptr;
    }
    const bool replacing = has_resident;
    if (found != resident_models_.end()
        && (found->second.blocked_generation == decoded_generation
            || (retry && pending->blocked))) {
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    }

    // Reserve the path key before any bounded integration work. A generation-
    // zero entry is a non-drawable sticky-refusal record for a cold source.
    if (found == resident_models_.end()) {
        try {
            found = resident_models_.try_emplace(
                source, ResidentModel{}).first;
        } catch (...) {
            ++stats_.model_integration_refusals;
            return nullptr;
        }
    }
    const auto block_generation = [&]() noexcept
        -> const ResidentModel* {
        found->second.blocked_generation = decoded_generation;
        if (retry) {
            pending->blocked = true;
        }
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    };
    const auto retry_later = [&]() noexcept -> const ResidentModel* {
        // Candidate/receipt capacity is recording-local pressure, not a defect
        // in this decoded generation. Preserve the current wall answer and let
        // a later command retry after earlier transactions reconcile.
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    };
    ++stats_.model_integration_attempts;
    const auto active_pending = static_cast<std::size_t>(std::count_if(
        pending_model_integrations_.begin(),
        pending_model_integrations_.end(),
        [](const PendingModelIntegration& integration) noexcept {
            return integration.candidate.generation != 0U;
        }));
    if ((!retry
            && retired_models_.size() + active_pending
                >= limits_.max_model_refresh_generations)
        || (!retry
            && pending_model_integrations_.size()
                >= limits_.max_model_refresh_generations)) {
        return retry_later();
    }

    const auto recording = commands.recording_token();
    if (!recording) {
        return retry_later();
    }

    const ModelAsset& decoded = *decoded_asset;
    const auto model_textures = texture_streamer_ ? std::span<const ModelTexture>{} :
        std::span<const ModelTexture>{decoded.textures};
    // Streams the candidate's geometry and (Basic) textures. Once all of it
    // has landed, or was written earlier in this recording, it is drawable
    // here; until then the wall keeps its current answer.
    const auto advance = [&](PendingModelIntegration& integration)
        -> const ResidentModel* {
        const bool geometry_drawable = integration.candidate.geometry
            && stream_geometry(*integration.candidate.geometry, commands, false);
        const TextureUpload textures = stream_model_textures(integration, commands);
        if (textures == TextureUpload::Failed) {
            integration.blocked = true;
            found->second.blocked_generation = decoded_generation;
            ++stats_.model_integration_refusals;
            return replacing ? &found->second : nullptr;
        }
        if (!geometry_drawable || textures != TextureUpload::Drawable) {
            ++stats_.models_pending;
            return replacing ? &found->second : nullptr;
        }
        integration.recording = recording;
        integration.blocked = false;
        ++stats_.model_integrations;
        return &integration.candidate;
    };
    if (retry && pending->candidate.generation == decoded_generation) {
        return advance(*pending);
    }

    core::u64 texture_bytes{};
    for (const ModelTexture& texture : model_textures) {
        if (!texture.valid()) {
            continue;
        }
        const auto bytes = texture_mip_bytes(texture, 0U);
        if (texture_bytes
                > std::numeric_limits<core::u64>::max() - bytes) {
            return block_generation();
        }
        texture_bytes += bytes;
    }
    if (texture_bytes > limits_.max_model_texture_bytes
        || model_texture_bytes_
            > limits_.max_model_texture_bytes - texture_bytes) {
        return block_generation();
    }

    ResidentModel resident{};
    resident.decoded_asset = decoded_asset;
    resident.bounds = decoded.bounds;
    resident.generation = decoded_generation;
    resident.texture_bytes = texture_bytes;
    try {
        // Both vectors reach their exact decoded sizes before the mesh arenas
        // are mutated or any command captures a GPU texture.
        resident.textures.reserve(model_textures.size());
        resident.parts.reserve(decoded.parts.size());
    } catch (...) {
        return block_generation();
    }
    const auto retain_partial = [&]() -> const ResidentModel* {
        const bool owns_resource = std::any_of(
            resident.textures.begin(), resident.textures.end(),
            [](const rhi::Texture& texture) noexcept {
                return static_cast<bool>(texture);
            });
        if (owns_resource) {
            model_texture_bytes_ += texture_bytes;
            retired_models_.push_back(std::move(resident));
            stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
        }
        return block_generation();
    };
    for (const ModelTexture& source_texture : model_textures) {
        rhi::Texture texture{};
        if (source_texture.valid()) {
            auto desc = sampled_desc(source_texture.width, source_texture.height);
            desc.format = model_texture_format(source_texture);
            desc.mip_levels = static_cast<core::u32>(source_texture.lower_mips.size() + 1U);
            auto created = device_->create_texture(desc);
            if (!created) {
                return retain_partial();
            }
            texture = std::move(created).value();
        }
        resident.textures.push_back(std::move(texture));
    }

    // Another renderer on this device may already hold this decoded
    // generation's geometry; then it is shared rather than written again.
    GeometryResidency::Handle geometry = geometry_->find(decoded);
    if (!geometry) {
        bool grew = false;
        geometry = geometry_->allocate(decoded_asset, commands, grew);
        stats_.geometry_uploaded = stats_.geometry_uploaded || grew;
    }
    if (!geometry) {
        // Slots free as retired generations are reclaimed; only a full arena
        // with nothing left to reclaim refuses this generation for good.
        if (!retired_models_.empty()) {
            const bool owns_resource = std::any_of(
                resident.textures.begin(), resident.textures.end(),
                [](const rhi::Texture& texture) noexcept {
                    return static_cast<bool>(texture);
                });
            if (owns_resource) {
                model_texture_bytes_ += texture_bytes;
                retired_models_.push_back(std::move(resident));
            }
            return retry_later();
        }
        return retain_partial();
    }
    const GeometrySpan span = geometry->span;
    resident.geometry = std::move(geometry);
    for (const auto& source_part : decoded.parts) {
        resident.parts.push_back(ResidentModelPart{
            MeshRange{
                span.first_index + source_part.first_index,
            source_part.index_count},
            source_part.material,
            source_part.has_material,
            source_part.base_color_texture,
            source_part.metallic_roughness_texture,
            source_part.normal_texture,
            source_part.occlusion_texture,
            source_part.emissive_texture,
            source_part.clearcoat_texture,
            source_part.clearcoat_roughness_texture,
            source_part.clearcoat_normal_texture,
            source_part.sheen_color_texture,
            source_part.sheen_roughness_texture,
            source_part.transmission_texture,
            source_part.specular_texture,
            source_part.specular_color_texture,
            source_part.anisotropy_texture,
            source_part.samplers});
    }
    if (retry) {
        if (pending->candidate.geometry || !pending->candidate.textures.empty()) {
            retire_candidate(*pending);
        }
        pending->candidate = std::move(resident);
        pending->recording = {};
        pending->blocked = false;
        pending->texture_next = 0U;
        pending->texture_writes.clear();
    } else {
        pending_model_integrations_.push_back(PendingModelIntegration{
            &found->second,
            std::move(resident),
            decoded_asset,
            {},
            decoded_generation,
            false});
        pending = std::prev(pending_model_integrations_.end());
    }
    model_texture_bytes_ += texture_bytes;
    stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    return advance(*pending);
}

} // namespace mediaserver::render
