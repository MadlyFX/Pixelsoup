#pragma once

#include "bake.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "stage_pick.hpp"

#include <ajfx/gpu/kernels.hpp>
#include <ajfx/math.hpp>
#include <ajfx/rigid_solver.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Turns pages into packages, away from the interface thread.
//
// The newest page wins: while one builds, later edits replace the queued one
// rather than lining up. A build compiles the page, compiles its kernels
// (cached by their source hash, so changing a value never waits on the shader
// compiler), and writes the preview package in place for the renderer to
// adopt. Afterwards the effect is run on the CPU reference, on a thread of
// its own, to find where its particles go and how many are alive at most, for
// framing and the budget readout, and to trace the paths of some of them.
// That run can take far longer than a build, so builds never wait for it: the
// preview follows a drag at the build's pace while the run under way finishes
// and the newest build is run next. The run uses the preview's own instance
// seed, so the paths are the very particles the renderer draws.
namespace ajfx::studio {

struct Bounds final {
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid{false};
};

struct BuildResult final {
    std::uint64_t revision{};
    // The particle system the package reads live values for (submit).
    Uid live{0U};
    Compiled compiled;
    // The preview package now holds this revision.
    bool written{false};
    // Why it could not be written (kernels, disk); compile problems are in
    // `compiled`.
    std::string error;
    // Milliseconds spent compiling kernels; 0 when they came from the cache.
    double kernel_ms{0.0};
    // An artist's file that could not be used, and what plays instead (a
    // soft dot, chips, nothing to collide with); empty when all were read.
    std::string asset_issue;
    // The outlines of the models mesh colliders use, by the path the page
    // names, in each model's own space, for the guides.
    std::map<std::string, std::vector<Vec3>, std::less<>> outlines;
};

// One particle's way through the preview loop. The effect is deterministic,
// so this is where that particle goes every time the loop plays.
struct ParticlePath final {
    Uid layer{};
    // The step it was born on; a point every Analysis::path_step steps from
    // then until it dies or the loop ends.
    std::uint64_t birth{};
    std::vector<Vec3> points;
    // Its colour at each point, colour-over-life and fades included.
    std::vector<Color> colors;
};

struct Analysis final {
    std::uint64_t revision{};
    // Nonzero: a quick run of this one particle system (focus_scene), with
    // only its paths. The whole scene's run follows with everything.
    Uid focus{0U};
    Bounds bounds;
    // Most particles alive at once, by layer uid.
    std::map<Uid, std::uint64_t> peak_alive;
    // The step with the most particles alive on the whole page: where the
    // effect costs the most to simulate and draw.
    std::uint64_t peak_step{0U};
    // Some of each layer's particles, spread evenly through its births.
    std::vector<ParticlePath> paths;
    std::uint64_t path_step{2U};
    // False when the effect was too large to run on the CPU: then there are
    // no bounds, peaks or paths.
    bool traced{true};
    // Where the page's solid things stand through the loop, every
    // `body_step` steps from step 0, for clicking on them in the preview.
    std::vector<StageBodies> bodies;
    std::uint64_t body_step{6U};
};

struct ExportResult final {
    bool ok{false};
    std::string message;
};

struct BakeResult final {
    bool ok{false};
    std::string message;
    // The bake, when it was taken.
    std::shared_ptr<const BakeRecord> record;
};

class PackageBuilder final {
public:
    // `instance_seed` is the preview placement's, for the CPU runs. Without
    // `analyse` a build is not run on the CPU afterwards (the budget's
    // probes only want the package).
    PackageBuilder(std::filesystem::path preview_package, std::uint32_t instance_seed, bool analyse = true);
    ~PackageBuilder();
    PackageBuilder(const PackageBuilder&) = delete;
    PackageBuilder& operator=(const PackageBuilder&) = delete;

    // `base` is the page's folder: a file the page names that is not where
    // it says is looked for there, so a page moved with its files still
    // finds them.
    // `live` is the particle system whose Motion values the package reads
    // from its placement (live_parameters); `focus` the one whose paths are
    // traced first, on their own, before the whole scene's. 0 for neither.
    void submit(Document document, std::uint64_t revision, std::filesystem::path base = {}, Uid live = 0U,
                Uid focus = 0U);
    // Only the CPU run of `document`: for an edit the package already shows
    // through its live values, whose paths and bounds still change. A
    // package build queued meanwhile still builds, of this document.
    void submit_analysis(Document document, std::uint64_t revision, std::filesystem::path base = {},
                         Uid focus = 0U);
    // `bake`, the newest bake, is written beside the package as
    // `<name>.ajfxcache` when it holds the exported page's baked layers as
    // they are; the result says why when it is not.
    void submit_export(Document document, std::filesystem::path path, std::filesystem::path base = {},
                       std::shared_ptr<const BakeRecord> bake = nullptr);
    // Bakes the page's baked layers (bake.hpp) once nothing else is waiting:
    // a preview or an export goes first, and a bake under way runs to its
    // end unless cancelled.
    void submit_bake(Document document, std::filesystem::path base = {});
    void cancel_bake();

    [[nodiscard]] std::optional<BuildResult> take_build();
    [[nodiscard]] std::optional<Analysis> take_analysis();
    [[nodiscard]] std::optional<ExportResult> take_export();
    [[nodiscard]] std::optional<BakeResult> take_bake();
    // The share of the bake under way, 0 to 1; negative when there is none.
    [[nodiscard]] double bake_progress() const noexcept { return bake_progress_.load(); }

    // A page is queued or building. The CPU run after it is not counted.
    [[nodiscard]] bool busy() const;
    [[nodiscard]] const std::filesystem::path& preview_path() const noexcept { return preview_path_; }

private:
    struct Request final {
        Document document;
        std::uint64_t revision{};
        std::filesystem::path base;
        Uid live{0U};
        Uid focus{0U};
        // False for submit_analysis: compiled for the CPU run, not written.
        bool package{true};
    };
    struct ExportRequest final {
        Document document;
        std::filesystem::path path;
        std::filesystem::path base;
        std::shared_ptr<const BakeRecord> bake;
    };
    struct BakeRequest final {
        Document document;
        std::filesystem::path base;
    };
    struct AnalysisRequest final {
        Request request;
        Compiled compiled;
        // focus_scene of the request's document, compiled, when it has a focus.
        std::optional<Compiled> focus;
    };
    // An artist's file, read and prepared, kept while the file is unchanged.
    struct Prepared final {
        std::filesystem::file_time_type time{};
        std::uintmax_t size{0U};
        std::optional<TextureData> texture;
        std::optional<MeshData> mesh;
        std::optional<DistanceField> field;
        std::string error;
    };
    struct Model final {
        std::filesystem::file_time_type time{};
        std::uintmax_t size{0U};
        ImportedMesh mesh;
        std::vector<Vec3> outline;
        std::string error;
    };

    void run();
    // The CPU runs, newest build first, on their own thread.
    void run_analyses();
    BuildResult build(const Request& request);
    ExportResult export_package(const ExportRequest& request);
    BakeResult bake_request(const BakeRequest& request);
    // One CPU run of `compiled` for `steps` steps. With `focus`, only that
    // layer's paths; otherwise everything, and it gives way to a newer
    // request when `yield` (its focus run was quick and has been shown).
    void analyse(const Request& request, const Compiled& compiled, std::uint64_t steps, Uid focus, bool yield);
    // Compiles a request's document and reads the files it names.
    Compiled prepare(const Document& document, const std::filesystem::path& base, std::string& issue,
                     std::map<std::string, std::vector<Vec3>, std::less<>>* outlines, Uid live = 0U);
    // Reads and prepares the artist's files `compiled` asks for, from the
    // cache where they are unchanged, into its textures, meshes and fields.
    // What cannot be read is replaced by what plays without it, and said in
    // `issue`.
    void resolve_assets(Compiled& compiled, const std::filesystem::path& base, std::string& issue,
                        std::map<std::string, std::vector<Vec3>, std::less<>>* outlines);
    const Model& model_at(const std::filesystem::path& path);
    // Kernels for the effect, from the cache or the compiler.
    std::optional<gpu::KernelSet> kernels_for(const Effect& effect, std::string& error, double& milliseconds);

    std::filesystem::path preview_path_;
    std::uint32_t instance_seed_{0U};
    bool analyse_{true};
    std::unique_ptr<gpu::ShaderCompiler> compiler_;
    // Run rigid physics layers for bakes and for the CPU analysis, each on
    // its own thread; made with the first.
    std::unique_ptr<RigidSolver> rigid_;
    std::unique_ptr<RigidSolver> analysis_rigid_;
    std::string compiler_error_;
    std::map<std::uint64_t, gpu::KernelSet> kernel_cache_;
    std::vector<std::uint64_t> kernel_order_;
    std::map<std::string, Prepared> assets_;
    std::vector<std::string> asset_order_;
    std::map<std::string, Model> models_;
    std::vector<std::string> model_order_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::optional<ExportRequest> pending_export_;
    std::optional<BuildResult> built_;
    std::optional<Analysis> analysed_;
    std::optional<ExportResult> exported_;
    std::optional<BakeRequest> pending_bake_;
    std::optional<BakeResult> baked_;
    std::atomic<double> bake_progress_{-1.0};
    std::atomic<bool> cancel_bake_{false};
    bool working_{false};
    bool stopping_{false};
    std::atomic<std::uint64_t> newest_revision_{0U};
    std::optional<AnalysisRequest> pending_analysis_;
    std::atomic<bool> analysis_waiting_{false};
    std::condition_variable analysis_wake_;
    std::atomic<bool> stop_analysis_{false};
    std::thread worker_;
    std::thread analyst_;
};

} // namespace ajfx::studio
