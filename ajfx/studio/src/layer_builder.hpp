#pragma once

#include "compile.hpp"
#include "document.hpp"

#include <ajfx/effect.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Compiles one layer of a page into one emitter: each block's modules placed
// in the stage and order the runtime needs, what several blocks say about one
// module merged, the layer's parameters declared. compile() (compile.cpp)
// builds each layer with one of these and sizes what comes out.
namespace ajfx::studio {

class LayerBuilder final {
public:
    // `world` is the page's (Document::world); `solids` the one world the
    // page's solid layers play in, which a particle layer meets and reads
    // (BuildCore); `blasts` the page's explosions, which it answers unless
    // it passes through.
    LayerBuilder(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters, PageWorld world = {},
                 SolidWorld solids = {}, std::vector<PageBlast> blasts = {});
    ~LayerBuilder();
    LayerBuilder(LayerBuilder&&) noexcept = default;
    LayerBuilder& operator=(LayerBuilder&&) noexcept = default;

    // Builds the emitter; on failure sets `error` and `error_block` of the report.
    std::optional<Emitter> build(LayerReport& report);

    // The cuts this layer's Break blocks asked for, in page order.
    [[nodiscard]] const std::vector<FractureCut>& cuts() const noexcept;
    // Room the layer needs beyond the bodies it places, because a body that
    // comes apart is replaced by its pieces. The runtime refuses a break
    // that will not fit and counts what it refused, so sizing for it here is
    // what keeps a page from half-breaking without saying why.
    [[nodiscard]] std::uint64_t extra_for_breaking() const noexcept;
    // The parameters this layer's blocks publish.
    [[nodiscard]] const std::vector<std::string>& published() const noexcept;
    // What a baked layer held rather than published.
    [[nodiscard]] const std::vector<LayerReport::Held>& held() const noexcept;
    // The artist's files the layer's blocks name.
    [[nodiscard]] const std::vector<AssetRequest>& requests() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ajfx::studio
