#include "package_builder.hpp"

#include "catalog.hpp"
#include "document_io.hpp"
#include "library.hpp"
#include "user_assets.hpp"

#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <ajfx/d3d12.hpp>
#include <ajfx/package.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <system_error>

namespace ajfx::studio {
namespace {

constexpr std::size_t kernel_cache_size = 24U;
// Larger effects are not analysed: the CPU reference would take too long to
// be worth waiting for, and framing falls back to a default view.
constexpr std::uint64_t analysis_particle_limit = 200000U;
// Artist's files kept prepared, and models kept read (a model's field is
// baked again when its scale changes, without reading the file again).
constexpr std::size_t asset_cache_size = 32U;
constexpr std::size_t model_cache_size = 8U;
constexpr std::size_t outline_segments = 3000U;

// Where a file the page names is: as named, else beside the page (under the
// name it was given, then by its file name alone), so a page moved with its
// files still finds them.
std::filesystem::path locate(const std::string& named, const std::filesystem::path& base) {
    std::error_code error;
    const std::filesystem::path path{named};
    if (std::filesystem::is_regular_file(path, error)) {
        return path;
    }
    if (!base.empty()) {
        if (path.is_relative() && std::filesystem::is_regular_file(base / path, error)) {
            return base / path;
        }
        if (std::filesystem::is_regular_file(base / path.filename(), error)) {
            return base / path.filename();
        }
    }
    return path;
}

bool read_image(const std::filesystem::path& path, ImportedImage& image, std::string& error) {
    const std::optional<std::vector<std::uint8_t>> bytes = read_file(path, error);
    if (!bytes) {
        return false;
    }
    mediaserver::render::DecodedFrame frame = mediaserver::render::decode_image_bytes(*bytes);
    if (!frame.valid() || frame.layout != mediaserver::render::DecodedPixelLayout::Rgba8 || frame.gpu ||
        frame.pixels.size() != static_cast<std::size_t>(frame.width) * frame.height * 4U) {
        error = "not a picture Studio can read (PNG, JPEG, TGA or BMP)";
        return false;
    }
    image.width = frame.width;
    image.height = frame.height;
    image.rgba = std::move(frame.pixels);
    return true;
}

bool read_model(const std::filesystem::path& path, ImportedMesh& mesh, std::string& error) {
    mediaserver::render::ModelAsset asset;
    if (!mediaserver::render::load_model_base(path.string(), asset)) {
        error = "not a model Studio can read (OBJ, glTF or GLB)";
        return false;
    }
    mesh.positions.reserve(asset.mesh.vertices.size());
    mesh.normals.reserve(asset.mesh.vertices.size());
    mesh.uvs.reserve(asset.mesh.vertices.size() * 2U);
    for (const auto& vertex : asset.mesh.vertices) {
        mesh.positions.push_back(Vec3{vertex.position.x, vertex.position.y, vertex.position.z});
        mesh.normals.push_back(Vec3{vertex.normal.x, vertex.normal.y, vertex.normal.z});
        mesh.uvs.push_back(vertex.u);
        mesh.uvs.push_back(vertex.v);
    }
    mesh.indices = asset.mesh.indices;
    if (mesh.indices.size() < 3U) {
        error = "the model has no triangles";
        return false;
    }
    return true;
}

template <class Map>
void bounded(Map& map, std::vector<std::string>& order, const std::string& key, std::size_t limit) {
    std::erase(order, key);
    order.push_back(key);
    while (order.size() > limit) {
        map.erase(order.front());
        order.erase(order.begin());
    }
}

// Paths traced in each layer at most: enough to see an effect's shape,
// few enough to draw every frame.
constexpr std::size_t paths_per_layer = 160U;

// Traces every `stride`-th particle by spawn index, doubling the stride (and
// dropping the paths it no longer picks) whenever there are too many, so the
// paths kept are spread evenly through the whole loop's births.
class PathTracer final {
public:
    explicit PathTracer(Uid layer) : layer_{layer} {}

    void sample(const EmitterState& state, std::uint64_t step) {
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            const std::uint32_t index = state.spawn_index[i];
            if (index % stride_ != 0U) {
                continue;
            }
            auto found = by_index_.find(index);
            if (found == by_index_.end()) {
                found = by_index_.emplace(index, ParticlePath{.layer = layer_, .birth = step}).first;
            }
            found->second.points.push_back(state.position[i]);
            found->second.colors.push_back(state.color[i]);
        }
        while (by_index_.size() > paths_per_layer) {
            stride_ *= 2U;
            std::erase_if(by_index_, [this](const auto& entry) { return entry.first % stride_ != 0U; });
        }
    }

    void finish(std::vector<ParticlePath>& into) {
        for (auto& [index, path] : by_index_) {
            if (path.points.size() >= 2U) {
                into.push_back(std::move(path));
            }
        }
    }

private:
    Uid layer_;
    std::uint32_t stride_{1U};
    std::map<std::uint32_t, ParticlePath> by_index_;
};

} // namespace

PackageBuilder::PackageBuilder(std::filesystem::path preview_package, std::uint32_t instance_seed)
    : preview_path_{std::move(preview_package)}, instance_seed_{instance_seed}, worker_{[this] { run(); }} {}

PackageBuilder::~PackageBuilder() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void PackageBuilder::submit(Document document, std::uint64_t revision, std::filesystem::path base) {
    {
        std::lock_guard lock{mutex_};
        pending_ = Request{std::move(document), revision, std::move(base)};
        newest_revision_.store(revision);
    }
    wake_.notify_all();
}

void PackageBuilder::submit_export(Document document, std::filesystem::path path, std::filesystem::path base) {
    {
        std::lock_guard lock{mutex_};
        pending_export_ = ExportRequest{std::move(document), std::move(path), std::move(base)};
    }
    wake_.notify_all();
}

std::optional<BuildResult> PackageBuilder::take_build() {
    std::lock_guard lock{mutex_};
    std::optional<BuildResult> out = std::move(built_);
    built_.reset();
    return out;
}

std::optional<Analysis> PackageBuilder::take_analysis() {
    std::lock_guard lock{mutex_};
    std::optional<Analysis> out = std::move(analysed_);
    analysed_.reset();
    return out;
}

std::optional<ExportResult> PackageBuilder::take_export() {
    std::lock_guard lock{mutex_};
    std::optional<ExportResult> out = std::move(exported_);
    exported_.reset();
    return out;
}

bool PackageBuilder::busy() const {
    std::lock_guard lock{mutex_};
    return working_ || pending_.has_value();
}

void PackageBuilder::run() {
    // The compiler lives on this thread: it is the only one that uses it.
    if (Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler()) {
        compiler_ = std::move(compiler).value();
    } else {
        compiler_error_ = compiler.error().message;
    }
    while (true) {
        std::optional<Request> request;
        std::optional<ExportRequest> export_request;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [&] { return stopping_ || pending_ || pending_export_; });
            if (stopping_) {
                return;
            }
            // The preview first: it is what the artist is looking at.
            if (pending_) {
                request = std::move(pending_);
                pending_.reset();
            } else {
                export_request = std::move(pending_export_);
                pending_export_.reset();
            }
            working_ = true;
        }
        if (request) {
            BuildResult result = build(*request);
            const Compiled compiled = result.compiled;
            const bool written = result.written;
            {
                std::lock_guard lock{mutex_};
                built_ = std::move(result);
            }
            if (written && compiled.effect) {
                analyse(*request, compiled);
            }
        } else if (export_request) {
            ExportResult result = export_package(*export_request);
            std::lock_guard lock{mutex_};
            exported_ = std::move(result);
        }
        std::lock_guard lock{mutex_};
        working_ = false;
    }
}

const PackageBuilder::Model& PackageBuilder::model_at(const std::filesystem::path& path) {
    const std::string key = path.string();
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    const std::uintmax_t size = error ? 0U : std::filesystem::file_size(path, error);
    if (const auto found = models_.find(key);
        found != models_.end() && found->second.time == time && found->second.size == size) {
        return found->second;
    }
    Model model;
    model.time = time;
    model.size = size;
    if (read_model(path, model.mesh, model.error)) {
        model.outline = mesh_outline(model.mesh, outline_segments);
    }
    models_[key] = std::move(model);
    bounded(models_, model_order_, key, model_cache_size);
    return models_.at(key);
}

void PackageBuilder::resolve_assets(Compiled& compiled, const std::filesystem::path& base, std::string& issue,
                                    std::map<std::string, std::vector<Vec3>, std::less<>>* outlines) {
    for (const AssetRequest& request : compiled.requests) {
        const std::filesystem::path path = locate(request.path, base);
        std::error_code error;
        const auto time = std::filesystem::last_write_time(path, error);
        const std::uintmax_t size = error ? 0U : std::filesystem::file_size(path, error);
        Prepared* prepared = nullptr;
        if (auto found = assets_.find(request.entry);
            found != assets_.end() && found->second.time == time && found->second.size == size) {
            prepared = &found->second;
        } else {
            Prepared fresh;
            fresh.time = time;
            fresh.size = size;
            if (error) {
                fresh.error = "cannot be found";
            } else if (request.kind == AssetRequest::Kind::Texture) {
                ImportedImage image;
                if (read_image(path, image, fresh.error)) {
                    fresh.texture = sprite_texture(image, fresh.error);
                }
            } else {
                const Model& model = model_at(path);
                fresh.error = model.error;
                if (model.error.empty() && request.kind == AssetRequest::Kind::Mesh) {
                    fresh.mesh = debris_mesh(model.mesh, fresh.error);
                } else if (model.error.empty()) {
                    if (std::optional<ColliderBake> bake =
                            bake_collider(model.mesh, request.scale, request.cells, request.solid, fresh.error)) {
                        fresh.field = std::move(bake->field);
                    }
                }
            }
            assets_[request.entry] = std::move(fresh);
            bounded(assets_, asset_order_, request.entry, asset_cache_size);
            prepared = &assets_.at(request.entry);
        }
        if (request.kind == AssetRequest::Kind::Field && outlines != nullptr) {
            if (const auto model = models_.find(path.string()); model != models_.end() && model->second.error.empty()) {
                (*outlines)[request.path] = model->second.outline;
            }
        }
        if (prepared->texture) {
            compiled.textures.emplace(request.entry, *prepared->texture);
            continue;
        }
        if (prepared->mesh) {
            compiled.meshes.emplace(request.entry, *prepared->mesh);
            continue;
        }
        if (prepared->field) {
            compiled.fields.emplace(request.entry, *prepared->field);
            continue;
        }
        // Not usable: the layer plays without it.
        const std::string reason = file_label(request.path) + " " + (prepared->error.empty() ? "cannot be read" : prepared->error);
        if (issue.empty()) {
            issue = reason;
        }
        if (!compiled.effect) {
            continue;
        }
        for (Emitter& emitter : compiled.effect->emitters) {
            if (request.kind == AssetRequest::Kind::Texture && emitter.renderer.texture == request.entry) {
                emitter.renderer.texture.clear();
                emitter.renderer.flipbook_columns = emitter.renderer.flipbook_rows = 1U;
                emitter.renderer.flipbook_blend = false;
            } else if (request.kind == AssetRequest::Kind::Mesh && emitter.renderer.mesh == request.entry) {
                emitter.renderer.mesh = "chip";
                if (const MeshData* chip = library_mesh("chip")) {
                    compiled.meshes.emplace("chip", *chip);
                }
            } else if (request.kind == AssetRequest::Kind::Field) {
                std::erase_if(emitter.update, [&](const UpdateModule& module) {
                    const auto* sdf = std::get_if<SdfCollision>(&module);
                    return sdf != nullptr && sdf->field == request.entry;
                });
            }
        }
    }
}

std::optional<gpu::KernelSet> PackageBuilder::kernels_for(const Effect& effect, std::string& error,
                                                          double& milliseconds) {
    const std::uint64_t key = gpu::kernel_source_hash(effect);
    if (const auto found = kernel_cache_.find(key); found != kernel_cache_.end()) {
        return found->second;
    }
    if (!compiler_) {
        error = "No shader compiler, so nothing can preview or export (" + compiler_error_ + ")";
        return std::nullopt;
    }
    const auto start = std::chrono::steady_clock::now();
    Result<gpu::KernelSet> kernels = gpu::compile_kernels(effect, *compiler_);
    milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!kernels) {
        error = kernels.error().message;
        return std::nullopt;
    }
    kernel_cache_[key] = kernels.value();
    kernel_order_.push_back(key);
    if (kernel_order_.size() > kernel_cache_size) {
        kernel_cache_.erase(kernel_order_.front());
        kernel_order_.erase(kernel_order_.begin());
    }
    return std::move(kernels).value();
}

BuildResult PackageBuilder::build(const Request& request) {
    BuildResult result;
    result.revision = request.revision;
    result.compiled = compile(request.document, Purpose::Preview);
    if (!result.compiled.effect) {
        return result;
    }
    resolve_assets(result.compiled, request.base, result.asset_issue, &result.outlines);
    std::optional<gpu::KernelSet> kernels = kernels_for(*result.compiled.effect, result.error, result.kernel_ms);
    if (!kernels) {
        return result;
    }
    std::vector<PackageEntry> extra = gpu::kernel_package_entries(*result.compiled.effect, *kernels);
    for (PackageEntry& entry : asset_entries(result.compiled)) {
        extra.push_back(std::move(entry));
    }
    Result<std::vector<std::uint8_t>> bytes = write_package(*result.compiled.effect, std::move(extra));
    if (!bytes) {
        result.error = bytes.error().message;
        return result;
    }
    // The renderer's package worker may be reading the last one; try again
    // for a moment rather than failing an edit.
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::string error;
        if (write_file(preview_path_, bytes.value(), error)) {
            result.written = true;
            result.error.clear();
            break;
        }
        result.error = error;
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    return result;
}

void PackageBuilder::analyse(const Request& request, const Compiled& compiled) {
    std::uint64_t capacity = 0U;
    for (const Emitter& emitter : compiled.effect->emitters) {
        capacity += emitter.capacity;
    }
    if (capacity > analysis_particle_limit) {
        Analysis untraced;
        untraced.revision = request.revision;
        untraced.traced = false;
        std::lock_guard lock{mutex_};
        analysed_ = std::move(untraced);
        return;
    }
    Result<Simulation> created = Simulation::create(*compiled.effect, instance_seed_, compiled.fields);
    if (!created) {
        return;
    }
    Simulation& simulation = created.value();
    const std::uint64_t steps = static_cast<std::uint64_t>(
        std::ceil(std::clamp(loop_seconds(request.document, compiled), 0.5F, 30.0F) *
                  static_cast<float>(compiled.effect->simulation_rate)));
    Analysis analysis;
    analysis.revision = request.revision;
    std::vector<std::uint64_t> peaks(compiled.effect->emitters.size(), 0U);
    // Emitters are the included layers, in page order.
    std::vector<PathTracer> tracers;
    for (const LayerReport& report : compiled.layers) {
        if (report.included && tracers.size() < peaks.size()) {
            tracers.emplace_back(report.layer);
        }
    }
    // Where particles are, sampled through the loop. The view frames the
    // middle 94% on each axis, so the body of the effect fills it rather than
    // the last wisp of smoke or one far-flung spark.
    std::array<std::vector<float>, 3> samples;
    constexpr std::size_t sample_limit = 60000U;
    float radius = 0.0F;
    std::uint64_t radius_count = 0U;
    for (std::uint64_t step = 0U; step < steps; ++step) {
        if (newest_revision_.load() != request.revision) {
            return; // edited again: this answer would be stale
        }
        simulation.step();
        for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
            const EmitterState& state = simulation.emitter(e);
            peaks[e] = std::max<std::uint64_t>(peaks[e], state.alive());
            // Points at the show frames the preview draws: frame f is step f.
            if (simulation.tick() % analysis.path_step == 0U && e < tracers.size()) {
                tracers[e].sample(state, simulation.tick());
            }
            if (step % 3U != 0U) {
                continue;
            }
            const std::size_t stride = std::max<std::size_t>(1U, state.alive() / 200U);
            for (std::size_t i = 0U; i < state.alive() && samples[0].size() < sample_limit; i += stride) {
                const Vec3 p = state.position[i];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                    continue;
                }
                samples[0].push_back(p.x);
                samples[1].push_back(p.y);
                samples[2].push_back(p.z);
                if (i < state.size.size() && std::isfinite(state.size[i])) {
                    radius += state.size[i] * 0.5F;
                    ++radius_count;
                }
            }
        }
    }
    if (!samples[0].empty()) {
        const float pad = radius_count != 0U ? radius / static_cast<float>(radius_count) : 0.05F;
        float low[3]{};
        float high[3]{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            std::vector<float>& values = samples[axis];
            const auto at = [&](double fraction) {
                const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U));
                std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
                return values[index];
            };
            low[axis] = at(0.03) - pad;
            high[axis] = at(0.97) + pad;
        }
        analysis.bounds = Bounds{Vec3{low[0], low[1], low[2]}, Vec3{high[0], high[1], high[2]}, true};
    }
    std::size_t index = 0U;
    for (const LayerReport& report : compiled.layers) {
        if (report.included && index < peaks.size()) {
            analysis.peak_alive[report.layer] = peaks[index++];
        }
    }
    for (PathTracer& tracer : tracers) {
        tracer.finish(analysis.paths);
    }
    std::lock_guard lock{mutex_};
    analysed_ = std::move(analysis);
}

ExportResult PackageBuilder::export_package(const ExportRequest& request) {
    ExportResult result;
    Compiled compiled = compile(request.document, Purpose::Export);
    if (!compiled.error.empty()) {
        result.message = "Not exported. " + compiled.error;
        return result;
    }
    // An export carries the artist's files or is not made: playing without
    // one would ship a different effect from the one on the page.
    std::string issue;
    resolve_assets(compiled, request.base, issue, nullptr);
    if (!issue.empty()) {
        result.message = "Not exported: " + issue;
        return result;
    }
    if (!compiled.effect) {
        result.message = "Not exported: there is nothing to play. Add a layer with a stream or burst.";
        return result;
    }
    std::string error;
    double milliseconds = 0.0;
    std::optional<gpu::KernelSet> kernels = kernels_for(*compiled.effect, error, milliseconds);
    if (!kernels) {
        result.message = "Not exported: " + error;
        return result;
    }
    std::vector<PackageEntry> extra = gpu::kernel_package_entries(*compiled.effect, *kernels);
    for (PackageEntry& entry : asset_entries(compiled)) {
        extra.push_back(std::move(entry));
    }
    Result<std::vector<std::uint8_t>> bytes = write_package(*compiled.effect, std::move(extra));
    if (!bytes) {
        result.message = "Not exported: " + bytes.error().message;
        return result;
    }
    if (!write_file(request.path, bytes.value(), error)) {
        result.message = "Not exported: " + error;
        return result;
    }
    result.ok = true;
    result.message = "Exported " + request.path.filename().string() + " (" +
                     std::to_string((bytes.value().size() + 1023U) / 1024U) + " KB)";
    return result;
}

} // namespace ajfx::studio
