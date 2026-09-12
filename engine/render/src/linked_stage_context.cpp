#include <mediaserver/render/linked_stage_context.hpp>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace mediaserver::render {
LinkedStageSample linked_stage_sample(const scene::Scene& s, core::u64 control) noexcept {
    LinkedStageSample out;
    out.scene=s.id.value(); out.control_frame=control; out.show_frame=s.show_frame;
    out.rate=s.show_rate; out.camera=s.camera_source.value();
    auto camera=s.camera;
    float aspect=s.canvas.height ? static_cast<float>(s.canvas.width)/static_cast<float>(s.canvas.height) : 16.0F/9.0F;
    scene::LensEncoders encoders;
    // The topmost drawing physical camera is the reference; otherwise use the
    // selected authored viewpoint. This is a reference, not another ICVFX view.
    for (const auto& tracked : s.tracked_cameras) if (tracked.drawing()) {
        camera=scene::tracked_view_camera(tracked.authored,tracked.pose,tracked.encoders);
        encoders=tracked.encoders; aspect=tracked.pose.aspect_ratio;
        out.camera=tracked.camera.value(); out.tracked=true; out.held=tracked.pose.held;
    }
    const auto back=scene::normalize(camera.eye-camera.target,{0,0,1});
    const auto right=scene::normalize(scene::cross(camera.up,back),{1,0,0});
    const auto up=scene::cross(back,right);
    out.camera_world={{right.x,right.y,right.z,0,up.x,up.y,up.z,0,back.x,back.y,back.z,0,camera.eye.x,camera.eye.y,camera.eye.z,1}};
    out.near_m=camera.near_plane; out.far_m=camera.far_plane;
    out.focus_m=camera.focus_distance_m; out.aperture=camera.aperture_f_number;
    out.sensor_height_mm=36.0F/aspect;
    out.focal_length_mm=out.sensor_height_mm/(2.0F*std::tan(camera.field_of_view*3.14159265358979323846F));
    if (out.tracked && camera.lens_profile) {
        const auto lens=scene::evaluate_lens(*camera.lens_profile,encoders);
        out.sensor_width_mm=lens.sensor_width_mm; out.sensor_height_mm=lens.sensor_height_mm;
        out.focal_length_mm=lens.focal_length_mm; out.calibrated=true;
    }
    return out;
}
std::string linked_stage_text(const LinkedStageSample& s, const scene::Mat4& alignment) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9);
    out << "MSSTAGE 1\n" << s.scene << ' ' << s.control_frame << ' ' << s.show_frame << ' '
        << s.rate.numerator << ' ' << s.rate.denominator << ' ' << s.camera << ' '
        << s.tracked << ' ' << s.held << ' ' << s.calibrated << '\n';
    for (float v : alignment.m) out << v << ' ';
    out << '\n'; for (float v : s.camera_world.m) out << v << ' ';
    out << '\n' << s.sensor_width_mm << ' ' << s.sensor_height_mm << ' ' << s.focal_length_mm << ' '
        << s.near_m << ' ' << s.far_m << ' ' << s.focus_m << ' ' << s.aperture << '\n';
    return out.str();
}
} // namespace mediaserver::render
