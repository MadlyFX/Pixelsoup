#pragma once

#include "compile.hpp"
#include "document.hpp"

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
// adopt. Afterwards the effect is run on the CPU reference to find where its
// particles go and how many are alive at most, for framing and the budget
// readout, and to trace the paths of some of them; a newer edit abandons
// that run. The run uses the preview's own instance seed, so the paths are
// the very particles the renderer draws.
namespace ajfx::studio {

struct Bounds final {
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid{false};
};

struct BuildResult final {
    std::uint64_t revision{};
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
    Bounds bounds;
    // Most particles alive at once, by layer uid.
    std::map<Uid, std::uint64_t> peak_alive;
    // Some of each layer's particles, spread evenly through its births.
    std::vector<ParticlePath> paths;
    std::uint64_t path_step{2U};
    // False when the effect was too large to run on the CPU: then there are
    // no bounds, peaks or paths.
    bool traced{true};
};

struct ExportResult final {
    bool ok{false};
    std::string message;
};

class PackageBuilder final {
public:
    // `instance_seed` is the preview placement's, for the CPU runs.
    PackageBuilder(std::filesystem::path preview_package, std::uint32_t instance_seed);
    ~PackageBuilder();
    PackageBuilder(const PackageBuilder&) = delete;
    PackageBuilder& operator=(const PackageBuilder&) = delete;

    // `base` is the page's folder: a file the page names that is not where
    // it says is looked for there, so a page moved with its files still
    // finds them.
    void submit(Document document, std::uint64_t revision, std::filesystem::path base = {});
    void submit_export(Document document, std::filesystem::path path, std::filesystem::path base = {});

    [[nodiscard]] std::optional<BuildResult> take_build();
    [[nodiscard]] std::optional<Analysis> take_analysis();
    [[nodiscard]] std::optional<ExportResult> take_export();

    // A page is queued or building.
    [[nodiscard]] bool busy() const;
    [[nodiscard]] const std::filesystem::path& preview_path() const noexcept { return preview_path_; }

private:
    struct Request final {
        Document document;
        std::uint64_t revision{};
        std::filesystem::path base;
    };
    struct ExportRequest final {
        Document document;
        std::filesystem::path path;
        std::filesystem::path base;
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
    BuildResult build(const Request& request);
    ExportResult export_package(const ExportRequest& request);
    void analyse(const Request& request, const Compiled& compiled);
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
    std::unique_ptr<gpu::ShaderCompiler> compiler_;
    // Runs rigid physics layers for the CPU analysis; made with the first.
    std::unique_ptr<RigidSolver> rigid_;
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
    bool working_{false};
    bool stopping_{false};
    std::atomic<std::uint64_t> newest_revision_{0U};
    std::thread worker_;
};

} // namespace ajfx::studio
