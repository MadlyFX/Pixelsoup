#include "package_builder.hpp"

#include "controls.hpp"
#include "delivery.hpp"
#include "document_io.hpp"
#include "library.hpp"
#include "performance.hpp"
#include "stage.hpp"
#include "scene.hpp"
#include "user_assets.hpp"

#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <ajfx/cache.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/package.hpp>
#include <ajfx/rigid.hpp>
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

// Paths traced of each item at most: enough to see an effect's shape,
// few enough to draw every frame.
constexpr std::size_t paths_per_layer = 160U;

// Traces every `stride`-th particle of one item by spawn index, doubling the
// stride (and dropping the paths it no longer picks) whenever there are too
// many, so the paths kept are spread evenly through the whole loop's births.
// An item is the particles of its emitter in its scope: the scene's Objects
// share one world (SolidWorld), each its own body kinds of it.
class PathTracer final {
public:
    PathTracer(Uid of_layer, std::size_t in_emitter, Scope within)
        : layer_{of_layer}, emitter_{in_emitter}, scope_{std::move(within)} {}

    [[nodiscard]] Uid layer() const noexcept { return layer_; }
    [[nodiscard]] std::size_t emitter() const noexcept { return emitter_; }
    [[nodiscard]] std::uint64_t peak() const noexcept { return peak_; }

    // The most of the layer's particles alive at once so far.
    void count(const EmitterState& state) {
        std::uint64_t alive = state.alive();
        if (!scope_.empty()) {
            alive = 0U;
            for (std::size_t i = 0U; i < state.alive(); ++i) {
                alive += mine(state, i) ? 1U : 0U;
            }
        }
        peak_ = std::max(peak_, alive);
    }

    void sample(const EmitterState& state, std::uint64_t step) {
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            const std::uint32_t index = state.spawn_index[i];
            if (index % stride_ != 0U || !mine(state, i)) {
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
    [[nodiscard]] bool mine(const EmitterState& state, std::size_t i) const {
        return scope_.empty() ||
               in_scope(scope_, state.spawn_index[i], i < state.body_kind.size() ? state.body_kind[i] : 0U);
    }

    Uid layer_;
    std::size_t emitter_{0U};
    Scope scope_;
    std::uint64_t peak_{0U};
    std::uint32_t stride_{1U};
    std::map<std::uint32_t, ParticlePath> by_index_;
};

} // namespace

PackageBuilder::PackageBuilder(std::filesystem::path preview_package, std::uint32_t instance_seed, bool analyse)
    : preview_path_{std::move(preview_package)}, instance_seed_{instance_seed}, analyse_{analyse},
      worker_{[this] { run(); }}, analyst_{[this] { run_analyses(); }} {}

PackageBuilder::~PackageBuilder() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
        // A bake under way stops at its next step rather than holding the
        // close up until it is done.
        cancel_bake_.store(true);
        stop_analysis_.store(true);
    }
    wake_.notify_all();
    analysis_wake_.notify_all();
    worker_.join();
    analyst_.join();
}

void PackageBuilder::submit(Document document, std::uint64_t revision, std::filesystem::path base, Uid live,
                            Uid focus) {
    {
        std::lock_guard lock{mutex_};
        pending_ = Request{std::move(document), revision, std::move(base), live, focus, true};
        newest_revision_.store(revision);
    }
    wake_.notify_all();
}

void PackageBuilder::submit_analysis(Document document, std::uint64_t revision, std::filesystem::path base,
                                     Uid focus) {
    {
        std::lock_guard lock{mutex_};
        // A package still queued is built all the same, of this newer page,
        // and keeps the system it was to read live.
        const bool package = pending_ && pending_->package;
        const Uid live = package ? pending_->live : 0U;
        pending_ = Request{std::move(document), revision, std::move(base), live, focus, package};
        // A package being written now is still the newest one.
        if (package) {
            newest_revision_.store(revision);
        }
    }
    wake_.notify_all();
}

void PackageBuilder::submit_export(Document document, std::filesystem::path path, std::filesystem::path base,
                                   std::shared_ptr<const BakeRecord> bake) {
    {
        std::lock_guard lock{mutex_};
        pending_export_ = ExportRequest{std::move(document), std::move(path), std::move(base), std::move(bake)};
    }
    wake_.notify_all();
}

void PackageBuilder::submit_bake(Document document, std::filesystem::path base) {
    {
        std::lock_guard lock{mutex_};
        pending_bake_ = BakeRequest{std::move(document), std::move(base)};
        cancel_bake_.store(false);
        // Under way from the moment it is asked for, so the button says so.
        bake_progress_.store(0.0);
    }
    wake_.notify_all();
}

void PackageBuilder::submit_settle(Document document, std::filesystem::path base) {
    {
        std::lock_guard lock{mutex_};
        pending_settle_ = SettleRequest{std::move(document), std::move(base)};
    }
    wake_.notify_all();
}

std::optional<SettleOutcome> PackageBuilder::take_settle() {
    std::lock_guard lock{mutex_};
    std::optional<SettleOutcome> out = std::move(settled_);
    settled_.reset();
    return out;
}

void PackageBuilder::cancel_bake() {
    std::lock_guard lock{mutex_};
    cancel_bake_.store(true);
    if (pending_bake_) {
        pending_bake_.reset();
        bake_progress_.store(-1.0);
    }
}

std::optional<BakeResult> PackageBuilder::take_bake() {
    std::lock_guard lock{mutex_};
    std::optional<BakeResult> out = std::move(baked_);
    baked_.reset();
    return out;
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
        std::optional<BakeRequest> bake_request_taken;
        std::optional<SettleRequest> settle_request_taken;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [&] {
                return stopping_ || pending_ || pending_export_ || pending_settle_ || pending_bake_;
            });
            if (stopping_) {
                return;
            }
            // The preview first: it is what the artist is looking at. A bake
            // last, since it is the long one.
            if (pending_) {
                request = std::move(pending_);
                pending_.reset();
            } else if (pending_export_) {
                export_request = std::move(pending_export_);
                pending_export_.reset();
            } else if (pending_settle_) {
                settle_request_taken = std::move(pending_settle_);
                pending_settle_.reset();
            } else {
                bake_request_taken = std::move(pending_bake_);
                pending_bake_.reset();
            }
            working_ = true;
        }
        if (request) {
            Compiled compiled;
            bool ready = false;
            if (request->package) {
                BuildResult result = build(*request);
                compiled = result.compiled;
                ready = result.written;
                std::lock_guard lock{mutex_};
                built_ = std::move(result);
            } else {
                std::string issue;
                compiled = prepare(request->document, request->base, issue, nullptr);
                ready = issue.empty() || !request->document.scene;
            }
            if (ready && compiled.effect && analyse_) {
                std::optional<Compiled> focus;
                if (request->focus != 0U) {
                    std::string issue;
                    Compiled alone = prepare(focus_scene(request->document, request->focus), request->base, issue, nullptr);
                    if (alone.effect && issue.empty()) {
                        focus = std::move(alone);
                    }
                }
                {
                    std::lock_guard lock{mutex_};
                    pending_analysis_ = AnalysisRequest{std::move(*request), std::move(compiled), std::move(focus)};
                    analysis_waiting_.store(true);
                }
                analysis_wake_.notify_all();
            }
        } else if (export_request) {
            ExportResult result = export_package(*export_request);
            std::lock_guard lock{mutex_};
            exported_ = std::move(result);
        } else if (settle_request_taken) {
            SettleOutcome outcome = settle_request(*settle_request_taken);
            std::lock_guard lock{mutex_};
            settled_ = std::move(outcome);
        } else if (bake_request_taken) {
            BakeResult result = bake_request(*bake_request_taken);
            std::lock_guard lock{mutex_};
            baked_ = std::move(result);
            bake_progress_.store(-1.0);
        }
        std::lock_guard lock{mutex_};
        working_ = false;
    }
}

void PackageBuilder::run_analyses() {
    while (true) {
        std::optional<AnalysisRequest> taken;
        {
            std::unique_lock lock{mutex_};
            analysis_wake_.wait(lock, [&] { return stopping_ || pending_analysis_; });
            if (stopping_) {
                return;
            }
            taken = std::move(pending_analysis_);
            pending_analysis_.reset();
            analysis_waiting_.store(false);
        }
        // Both runs cover the whole scene's loop, so the focus run's paths
        // are the very ones the whole run traces.
        const std::uint64_t steps = static_cast<std::uint64_t>(
            std::ceil(std::clamp(loop_seconds(taken->request.document), 0.5F, 30.0F) *
                      static_cast<float>(taken->compiled.effect->simulation_rate)));
        if (taken->focus) {
            analyse(taken->request, *taken->focus, steps, taken->request.focus, false);
        }
        analyse(taken->request, taken->compiled, steps, 0U, taken->focus.has_value());
    }
}

Compiled PackageBuilder::prepare(const Document& document, const std::filesystem::path& base, std::string& issue,
                                 std::map<std::string, std::vector<Vec3>, std::less<>>* outlines, Uid live) {
    Compiled compiled = compile(document, live);
    if (compiled.effect) {
        resolve_assets(compiled, base, issue, outlines);
    }
    return compiled;
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
            } else {
                const Model& model = model_at(path);
                fresh.error = model.error;
                if (model.error.empty() && request.kind == AssetRequest::Kind::Mesh) {
                    fresh.mesh = debris_mesh(model.mesh, fresh.error);
                } else if (model.error.empty()) {
                    const bool placed = request.fit || request.turn != Vec3{};
                    if (std::optional<ColliderBake> bake =
                            bake_collider(placed ? placed_mesh(model.mesh, request.fit, request.turn) : model.mesh,
                                          request.scale, request.cells, request.solid, fresh.error)) {
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
        // Not usable: the item plays without it.
        const std::string reason = file_label(request.path) + " " + (prepared->error.empty() ? "cannot be read" : prepared->error);
        if (issue.empty()) {
            issue = reason;
        }
        if (!compiled.effect) {
            continue;
        }
        for (Emitter& emitter : compiled.effect->emitters) {
            if (request.kind == AssetRequest::Kind::Mesh && emitter.renderer.mesh == request.entry) {
                emitter.renderer.mesh = "slab";
                if (const MeshData* slab = library_mesh("slab")) {
                    compiled.meshes.emplace("slab", *slab);
                }
            } else if (request.kind == AssetRequest::Kind::Field) {
                std::erase_if(emitter.update, [&](const UpdateModule& module) {
                    const auto* sdf = std::get_if<SdfCollision>(&module);
                    return sdf != nullptr && sdf->field == request.entry;
                });
            }
        }
    }
    // The models are all in now, so what asked to be cut can be.
    make_cuts(compiled, issue);
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
    result.live = request.live;
    result.compiled = prepare(request.document, request.base, result.asset_issue, &result.outlines, request.live);
    if (!result.compiled.effect) {
        return result;
    }
    if (request.document.scene && !result.asset_issue.empty()) {
        result.error = result.asset_issue;
        return result;
    }
    if (!result.compiled.effect) {
        // A cut the models made could not be held to the effect's rules:
        // what it was is the asset issue, and nothing new is written.
        result.error = result.asset_issue.empty() ? "the pieces cut from a model could not be used" : result.asset_issue;
        return result;
    }
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
        if (newest_revision_.load() != request.revision) { return result; }
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

void PackageBuilder::analyse(const Request& request, const Compiled& compiled, std::uint64_t steps, Uid focus,
                             bool yield) {
    std::uint64_t capacity = 0U;
    for (const Emitter& emitter : compiled.effect->emitters) {
        capacity += emitter.capacity;
    }
    if (capacity > analysis_particle_limit) {
        Analysis untraced;
        untraced.revision = request.revision;
        untraced.focus = focus;
        untraced.traced = false;
        std::lock_guard lock{mutex_};
        analysed_ = std::move(untraced);
        return;
    }
    if (has_rigid_physics(*compiled.effect) && !analysis_rigid_) {
        analysis_rigid_ = rigid::make_jolt_solver();
    }
    Result<Simulation> created = Simulation::create(
        *compiled.effect, instance_seed_,
        SimulationOptions{.fields = compiled.fields, .meshes = compiled.meshes, .rigid = analysis_rigid_.get()});
    if (!created) {
        return;
    }
    Simulation& simulation = created.value();
    // The scene's performance plays in the run as it does in the preview,
    // so a dropped body can be clicked and is framed.
    const std::shared_ptr<const ActionList> actions =
        request.document.scene ? performance_actions(request.document.scene->performance, act_targets(compiled))
                               : nullptr;
    simulation.reset(nullptr, actions.get());
    Analysis analysis;
    analysis.revision = request.revision;
    analysis.focus = focus;
    // Each item that plays, by the emitter it plays in and its scope there.
    std::vector<PathTracer> tracers;
    for (const ItemReport& report : compiled.items) {
        if (!report.included || (focus != 0U && report.item != focus)) {
            continue;
        }
        for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
            if (compiled.effect->emitters[e].name == report.emitter) {
                tracers.emplace_back(report.item, e, report.scope);
                break;
            }
        }
    }
    // Where particles are, sampled through the loop. The view frames the
    // middle 94% on each axis, so the body of the effect fills it rather than
    // the last wisp of smoke or one far-flung spark.
    std::array<std::vector<float>, 3> samples;
    constexpr std::size_t sample_limit = 60000U;
    // Where the solid things stand, for picking them in the preview: from
    // step 0, where a pile already stands, then every few steps.
    const std::optional<std::size_t> world = solid_world_index(compiled);
    if (world) {
        analysis.bodies.push_back(StageBodies{simulation.tick(), stage_bodies(compiled, simulation.emitter(*world))});
    }
    float radius = 0.0F;
    std::uint64_t radius_count = 0U;
    std::uint64_t most_alive = 0U;
    for (std::uint64_t step = 0U; step < steps; ++step) {
        // A newer edit does not stop the run: during a drag every run would
        // be stopped and the paths would never follow it.
        if (stop_analysis_.load() || (yield && analysis_waiting_.load())) {
            return;
        }
        simulation.step(nullptr, actions.get());
        std::uint64_t alive = 0U;
        for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
            alive += simulation.emitter(e).alive();
        }
        if (alive > most_alive) {
            most_alive = alive;
            analysis.peak_step = simulation.tick();
        }
        for (PathTracer& tracer : tracers) {
            const EmitterState& state = simulation.emitter(tracer.emitter());
            tracer.count(state);
            // Points at the show frames the preview draws: frame f is step f.
            if (simulation.tick() % analysis.path_step == 0U) {
                tracer.sample(state, simulation.tick());
            }
        }
        if (world && simulation.tick() % analysis.body_step == 0U) {
            analysis.bodies.push_back(
                StageBodies{simulation.tick(), stage_bodies(compiled, simulation.emitter(*world))});
        }
        for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
            const EmitterState& state = simulation.emitter(e);
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
    for (const PathTracer& tracer : tracers) {
        analysis.peak_alive[tracer.layer()] = tracer.peak();
    }
    for (PathTracer& tracer : tracers) {
        tracer.finish(analysis.paths);
    }
    std::lock_guard lock{mutex_};
    analysed_ = std::move(analysis);
}

ExportResult PackageBuilder::export_package(const ExportRequest& request) {
    ExportResult result;
    Compiled compiled = compile(request.document);
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
        result.message = "Not exported: there is nothing to play. Add an object or a particle system.";
        return result;
    }
    const std::vector<CacheMode> modes = bake_modes(request.document, compiled);
    const bool any_baked = std::any_of(modes.begin(), modes.end(), [](CacheMode mode) { return mode != CacheMode::Live; });
    std::string error;
    double milliseconds = 0.0;
    std::optional<gpu::KernelSet> kernels = kernels_for(*compiled.effect, error, milliseconds);
    if (!kernels) {
        result.message = "Not exported: " + error;
        return result;
    }
    // A scene is delivered with its performance's cache, checked as the
    // media server checks it (delivery.hpp).
    if (request.document.scene) {
        Result<Delivery> delivery = deliver(request.document, compiled, *kernels, request.bake.get());
        if (!delivery) {
            result.message = "Not exported. " + delivery.error().message;
            return result;
        }
        if (!write_delivery(delivery.value(), request.path, error)) {
            result.message = "Not exported: " + error;
            return result;
        }
        result.ok = true;
        result.message = "Exported " + request.path.filename().string() + " (" +
                         bytes_text(delivery.value().package.size()) + ")";
        if (delivery.value().played) {
            result.message += " with its cache, " + cache_path_for(request.path).filename().string() + " (" +
                              bytes_text(delivery.value().cache.size()) + ")";
        }
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
    // The cache goes beside it, if it is this page's: opening it against the
    // exported effect is the very check a host will make.
    if (request.bake) {
        Result<Cache> fits = Cache::open(request.bake->bytes, *compiled.effect, request.bake->instance_seed, {},
                                         compiled.meshes, compiled.fields);
        if (fits) {
            std::filesystem::path cache_path = request.path;
            cache_path.replace_extension(".ajfxcache");
            if (write_file(cache_path, request.bake->bytes, error)) {
                result.message += " with its cache, " + cache_path.filename().string() + " (" +
                                  bytes_text(request.bake->bytes.size()) + ")";
            } else {
                result.message += ". Its cache was not written: " + error;
            }
        } else if (any_baked) {
            result.message += ". Its cache was not written, since " + fits.error().message +
                              ": bake again, then export";
        }
    } else if (any_baked) {
        result.message += ". No cache was written: the baked items' cache is missing or out of date, so press "
                          "Bake, then export";
    }
    return result;
}

SettleOutcome PackageBuilder::settle_request(const SettleRequest& request) {
    SettleOutcome outcome;
    if (!request.document.scene) {
        outcome.message = "Only a scene has a stage";
        return outcome;
    }
    outcome.before = request.document.scene->performance;
    // As a bake does: the exported effect with its files read and its cuts
    // made, since that is the world the stage stands in.
    Compiled compiled = compile(request.document);
    if (!compiled.error.empty()) {
        outcome.message = "Not settled. " + compiled.error;
        return outcome;
    }
    std::string issue;
    resolve_assets(compiled, request.base, issue, nullptr);
    if (!issue.empty()) {
        outcome.message = "Not settled: " + issue;
        return outcome;
    }
    if (compiled.effect && has_rigid_physics(*compiled.effect) && !rigid_) {
        rigid_ = rigid::make_jolt_solver();
    }
    Document settled = request.document;
    outcome.result = settle_stage(settled, compiled, rigid_.get(), SettleOptions{.instance_seed = instance_seed_});
    if (!outcome.result.refusal.empty()) {
        outcome.message = outcome.result.refusal;
        return outcome;
    }
    outcome.after = settled.scene->performance;
    outcome.ok = true;
    outcome.message = "Settled " + std::to_string(outcome.result.settled) + (outcome.result.settled == 1U ? " thing" : " things");
    if (outcome.result.moving != 0U) {
        outcome.message += "; " + std::to_string(outcome.result.moving) + " still moving after the time allowed";
    }
    return outcome;
}

BakeResult PackageBuilder::bake_request(const BakeRequest& request) {
    BakeResult result;
    Compiled compiled = compile(request.document);
    if (!compiled.error.empty()) {
        result.message = "Not baked. " + compiled.error;
        return result;
    }
    // A bake reads the artist's files and makes the cuts, as the export
    // does: it is the exported effect that the cache must belong to.
    std::string issue;
    resolve_assets(compiled, request.base, issue, nullptr);
    if (!issue.empty()) {
        result.message = "Not baked: " + issue;
        return result;
    }
    if (compiled.effect && has_rigid_physics(*compiled.effect) && !rigid_) {
        rigid_ = rigid::make_jolt_solver();
    }
    Result<BakeRecord> baked =
        bake_scene(request.document, compiled, instance_seed_, rigid_.get(), [this](double share) {
            bake_progress_.store(share);
            return !cancel_bake_.load();
        });
    if (!baked) {
        result.message = cancel_bake_.load() ? std::string{"Bake cancelled"} : "Not baked: " + baked.error().message;
        return result;
    }
    auto record = std::make_shared<BakeRecord>(std::move(baked).value());
    result.ok = true;
    result.message = "Baked " + std::to_string(record->frames) + " steps into " + bytes_text(record->report.bytes) +
                     " in " + format_number(static_cast<float>(record->seconds), 1) + " s";
    result.record = std::move(record);
    return result;
}

} // namespace ajfx::studio
