#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <mediaserver/render/geometry_visibility.hpp>
#include <mediaserver/scene/math.hpp>
#include <stdexcept>

namespace {
namespace scene = mediaserver::scene;
namespace render = mediaserver::render;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void geometry_contract() {
    scene::Scene frame;
    frame.canvas = {1000U, 1000U};
    frame.camera.field_of_view =
        0.25F; // 90 degrees: ten metres wide at depth five.
    frame.camera.far_plane = 100.0F;
    const render::ModelBounds unit{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    const render::GeometryFrusta frusta{frame};
    require(frusta.visible(scene::translation({0.0F, 0.0F, -5.0F}), unit),
            "central box was culled");
    require(!frusta.visible(scene::translation({0.0F, 0.0F, 5.0F}), unit),
            "behind-eye box survived");
    require(!frusta.visible(scene::translation({20.0F, 0.0F, -5.0F}), unit),
            "right box survived");
    require(!frusta.visible(scene::translation({-20.0F, 0.0F, -5.0F}), unit),
            "left box survived");
    require(!frusta.visible(scene::translation({0.0F, 20.0F, -5.0F}), unit),
            "upper box survived");
    require(!frusta.visible(scene::translation({0.0F, -20.0F, -5.0F}), unit),
            "lower box survived");
    require(!frusta.visible(scene::translation({0.0F, 0.0F, -110.0F}), unit),
            "far box survived");
    require(frusta.visible(scene::identity(), unit),
            "near-plane crossing culled");
    require(frusta.visible(scene::translation({5.0F, 0.0F, -5.0F}), unit),
            "edge crossing culled");
    const auto mirrored =
        scene::multiply(scene::translation({0.0F, 0.0F, -5.0F}),
                        scene::scaling({-2.0F, 0.25F, 3.0F}));
    require(frusta.visible(mirrored, unit), "nonuniform mirrored bound culled");
    auto broken = unit;
    broken.minimum.x = std::numeric_limits<float>::quiet_NaN();
    require(frusta.visible(scene::translation({20.0F, 0.0F, -5.0F}), broken),
            "unknown bounds must be conservative");

    const render::ModelBounds flat{{-0.5F, -0.5F, 0.0F}, {0.5F, 0.5F, 0.0F}};
    const std::array<render::ModelLod, 3U> levels{
        {{0U, 1U, 0.0F}, {1U, 1U, 120.0F}, {2U, 1U, 60.0F}}};
    require(
        std::abs(render::projected_model_pixels(
                     frame, scene::translation({0.0F, 0.0F, -5.0F}), flat) -
                 100.0F) < 0.02F,
        "one metre at five metres in a 90 degree 1000px view must cover 100px");
    require(render::select_model_lod(frame,
                                     scene::translation({0.0F, 0.0F, -2.0F}),
                                     flat, levels) == 0U,
            "near LOD");
    require(render::select_model_lod(frame,
                                     scene::translation({0.0F, 0.0F, -5.0F}),
                                     flat, levels) == 1U,
            "middle LOD");
    require(render::select_model_lod(frame,
                                     scene::translation({0.0F, 0.0F, -10.0F}),
                                     flat, levels) == 2U,
            "far LOD");
    frame.show_frame = 1000U;
    frame.document_version = mediaserver::doc::Version{71U};
    require(render::select_model_lod(frame,
                                     scene::translation({0.0F, 0.0F, -5.0F}),
                                     flat, levels) == 1U,
            "seek/edit changed LOD");
    require(render::select_model_lod(frame, scene::identity(), unit, levels) ==
                0U,
            "near clipping must keep finest");
    frame.renderer_settings.model_lod = false;
    require(render::select_model_lod(frame,
                                     scene::translation({0.0F, 0.0F, -10.0F}),
                                     flat, levels) == 0U,
            "disabled LOD");

    // Every policy bit changes both readiness evidence and evaluated identity.
    for (auto member : {&scene::RendererSettings::frustum_culling,
                        &scene::RendererSettings::geometry_instancing,
                        &scene::RendererSettings::model_lod}) {
        scene::Scene original;
        auto changed = original;
        changed.renderer_settings.*member = false;
        require(scene::renderer_settings_digest(original.renderer_settings) !=
                    scene::renderer_settings_digest(changed.renderer_settings),
                "profile omitted geometry policy");
        require(scene::digest(original) != scene::digest(changed),
                "scene omitted geometry policy");
    }
}
} // namespace
int main() {
    try {
        geometry_contract();
        std::cout << "[pass] full-canvas geometry contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
