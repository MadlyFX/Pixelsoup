#include <mediaserver/gui/ui_renderer.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/ui_frag_dxil.h>
#include <mediaserver/shaders/ui_vert_dxil.h>
#endif

#include <algorithm>
#include <cstring>
#include <utility>

namespace mediaserver::gui {
namespace {

namespace rhi = mediaserver::rhi;
using render::RenderError;

// Ample for a dense operator screen; a frame past this is refused rather than
// grown on the draw path.
constexpr core::u32 max_ui_vertices = 1U << 18U;

// Matches ui.vert.hlsl's ViewConstants (b0) and BatchConstants (b1).
struct ViewConstants final {
    float target_width{};
    float target_height{};
    float pad0{};
    float pad1{};
};
struct BatchConstants final {
    core::u32 first_vertex{};
    core::u32 pad_a{};
    core::u32 pad_b{};
    core::u32 pad_c{};
    float clip_min_x{};
    float clip_min_y{};
    float clip_max_x{};
    float clip_max_y{};
};

template <typename T>
void append(std::vector<core::u8>& out, const T& value) {
    const auto* const bytes = reinterpret_cast<const core::u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

} // namespace

// ---------------------------------------------------------------------------
// UiDrawList

void UiDrawList::set_white_texel(float u, float v) noexcept {
    white_u_ = u;
    white_v_ = v;
}

void UiDrawList::reset() noexcept {
    vertices_.clear();
    batches_.clear();
    clip_stack_.clear();
}

void UiDrawList::push_clip(float x, float y, float width, float height) {
    Clip clip{x, y, x + width, y + height};
    if (!clip_stack_.empty()) {
        const Clip& parent = clip_stack_.back();
        clip.min_x = std::max(clip.min_x, parent.min_x);
        clip.min_y = std::max(clip.min_y, parent.min_y);
        clip.max_x = std::min(clip.max_x, parent.max_x);
        clip.max_y = std::min(clip.max_y, parent.max_y);
    }
    clip_stack_.push_back(clip);
}

void UiDrawList::pop_clip() {
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
}

UiDrawList::Clip UiDrawList::current_clip() const noexcept {
    if (!clip_stack_.empty()) {
        return clip_stack_.back();
    }
    return Clip{-1.0e6F, -1.0e6F, 1.0e6F, 1.0e6F};
}

void UiDrawList::add_rect(
    float x, float y, float width, float height, UiColor color) {
    quad(x, y, width, height, white_u_, white_v_, white_u_, white_v_, color, nullptr);
}

void UiDrawList::add_textured(
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    UiColor color) {
    quad(x, y, width, height, u0, v0, u1, v1, color, nullptr);
}

void UiDrawList::add_image(
    float x,
    float y,
    float width,
    float height,
    const rhi::Texture& texture,
    UiColor tint) {
    // The whole texture across the rectangle. Its own uv runs top-left down,
    // the same way the UI's pixels do, so no flip is needed.
    quad(x, y, width, height, 0.0F, 0.0F, 1.0F, 1.0F, tint, &texture);
}

void UiDrawList::quad(
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    UiColor color,
    const rhi::Texture* texture) {
    const Clip clip = current_clip();
    // Fully-clipped quads contribute nothing; skip them so an offscreen widget
    // costs no vertices.
    if (clip.min_x >= clip.max_x || clip.min_y >= clip.max_y) {
        return;
    }
    // A batch runs until either the clip or the texture changes: both are bound
    // per batch, and the texture additionally costs a pass, so they break the
    // run together rather than separately.
    const bool same_run = !batches_.empty()
        && batches_.back().clip_min_x == clip.min_x
        && batches_.back().clip_min_y == clip.min_y
        && batches_.back().clip_max_x == clip.max_x
        && batches_.back().clip_max_y == clip.max_y
        && batches_.back().texture == texture;
    if (!same_run) {
        batches_.push_back(UiBatch{
            static_cast<core::u32>(vertices_.size()),
            0U,
            clip.min_x,
            clip.min_y,
            clip.max_x,
            clip.max_y,
            texture});
    }

    const UiVertex tl{x, y, u0, v0, color.r, color.g, color.b, color.a};
    const UiVertex tr{x + width, y, u1, v0, color.r, color.g, color.b, color.a};
    const UiVertex br{
        x + width, y + height, u1, v1, color.r, color.g, color.b, color.a};
    const UiVertex bl{x, y + height, u0, v1, color.r, color.g, color.b, color.a};
    vertices_.push_back(tl);
    vertices_.push_back(tr);
    vertices_.push_back(br);
    vertices_.push_back(tl);
    vertices_.push_back(br);
    vertices_.push_back(bl);
    batches_.back().vertex_count += 6U;
}

// ---------------------------------------------------------------------------
// UiRenderer

UiRenderer::UiRenderer(
    rhi::GraphicsPipeline pipeline,
    rhi::Sampler sampler,
    rhi::StorageBuffer vertices) noexcept
    : pipeline_(std::move(pipeline))
    , sampler_(std::move(sampler))
    , vertex_buffer_(std::move(vertices)) {}

bool UiRenderer::shaders_available() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

core::Result<UiRenderer, RenderError> UiRenderer::create(
    rhi::Device& device, rhi::TextureFormat target_format) {
    using Result = core::Result<UiRenderer, RenderError>;
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_ui_vert_dxil),
        sizeof(mediaserver_ui_vert_dxil)};
    vertex.storage_buffer_count = 1U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_ui_frag_dxil),
        sizeof(mediaserver_ui_frag_dxil)};
    fragment.sampler_count = 1U;

    // draw_pulled always binds a depth target, so the pipeline must declare one
    // to match the pass. The UI never occludes itself: depth is enabled only to
    // satisfy that match, with writes off and a GREATER test the far-plane z of
    // every UI vertex passes against the 0.0-cleared buffer.
    const rhi::DepthState depth{true, false, true};
    auto pipeline =
        device.create_pipeline(vertex, fragment, target_format, true, depth);
    if (!pipeline) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return Result::failure(RenderError::Unavailable);
    }
    auto vertices = device.create_storage_buffer(
        max_ui_vertices * static_cast<core::u32>(sizeof(UiVertex)));
    if (!vertices) {
        return Result::failure(RenderError::AllocationFailed);
    }
    return Result::success(UiRenderer{
        std::move(pipeline).value(),
        std::move(sampler).value(),
        std::move(vertices).value()});
#else
    static_cast<void>(device);
    static_cast<void>(target_format);
    return Result::failure(RenderError::ShaderFormatUnsupported);
#endif
}

bool UiRenderer::upload_atlas(
    rhi::Device& device,
    rhi::CommandList& commands,
    std::span<const core::u8> rgba,
    core::u32 width,
    core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    if (!texture) {
        return false;
    }
    atlas_ = std::move(texture).value();
    return commands.upload(atlas_, rgba);
}

bool UiRenderer::render(
    rhi::Device& device,
    rhi::CommandList& commands,
    rhi::Texture& target,
    rhi::Color clear,
    const UiDrawList& draw_list,
    core::u32 width,
    core::u32 height) {
    if (atlas_.backend_handle() == nullptr) {
        return false; // upload_atlas must run first
    }

    // The pulled draw requires a depth target of the colour's size; the UI never
    // tests depth, but the entrypoint binds one. Kept sized to the target.
    if (depth_.backend_handle() == nullptr || depth_width_ != width
        || depth_height_ != height) {
        rhi::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = rhi::TextureFormat::Depth32Float;
        desc.depth_target = true;
        auto depth = device.create_texture(desc);
        if (!depth) {
            return false;
        }
        depth_ = std::move(depth).value();
        depth_width_ = width;
        depth_height_ = height;
    }

    const auto& vertices = draw_list.vertices();
    if (vertices.size() > max_ui_vertices) {
        return false;
    }
    vertex_scratch_.resize(vertices.size() * sizeof(UiVertex));
    if (!vertices.empty()) {
        std::memcpy(
            vertex_scratch_.data(), vertices.data(), vertex_scratch_.size());
        if (!commands.upload(vertex_buffer_, vertex_scratch_)) {
            return false;
        }
    }

    const ViewConstants view{
        static_cast<float>(width), static_cast<float>(height), 0.0F, 0.0F};
    const rhi::StorageBuffer* const vertex_buffers[] = {&vertex_buffer_};

    const auto record = [&](const rhi::Texture& texture, bool first) {
        rhi::PulledDraw draw{};
        draw.pipeline = &pipeline_;
        draw.vertex_buffers = vertex_buffers;
        draw.vertex_uniform = std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(&view), sizeof(view)};
        draw.batch_uniforms = batch_scratch_;
        draw.batch_uniform_stride = sizeof(BatchConstants);
        draw.batches = pass_batches_;
        draw.texture = &texture;
        draw.sampler = &sampler_;
        // The first pass clears; the rest compose over what it left, so the
        // painter's order across textures is still the CPU draw order.
        draw.load = !first;
        // Depth is only present to match the pass. It is never written, but a
        // later pass loads and tests against it, so it has to survive.
        draw.store_depth = true;
        return commands.draw_pulled(target, clear, depth_, 0.0F, draw);
    };

    // The RHI binds one texture per pass, so consecutive batches are grouped by
    // the texture they sample and each group becomes a pass. A UI of nothing but
    // text and fills is one pass, exactly as before; a panel showing an engine
    // target costs one more, and no copy.
    const auto& batches = draw_list.batches();
    bool first_pass = true;
    std::size_t index = 0U;
    while (index < batches.size()) {
        const rhi::Texture* const texture = batches[index].texture;
        batch_scratch_.clear();
        pass_batches_.clear();

        std::size_t end = index;
        while (end < batches.size() && batches[end].texture == texture) {
            const UiBatch& batch = batches[end];
            ++end;
            if (batch.vertex_count == 0U) {
                continue;
            }
            BatchConstants constants{};
            constants.first_vertex = batch.first_vertex;
            constants.clip_min_x = batch.clip_min_x;
            constants.clip_min_y = batch.clip_min_y;
            constants.clip_max_x = batch.clip_max_x;
            constants.clip_max_y = batch.clip_max_y;
            append(batch_scratch_, constants);
            pass_batches_.push_back(rhi::DrawBatch{batch.vertex_count, 1U});
        }
        index = end;

        if (pass_batches_.empty()) {
            continue;
        }
        if (!record(texture != nullptr ? *texture : atlas_, first_pass)) {
            return false;
        }
        first_pass = false;
    }

    if (first_pass) {
        // Nothing to draw, but the target still owes its clear.
        batch_scratch_.clear();
        pass_batches_.clear();
        return record(atlas_, true);
    }
    return true;
}

} // namespace mediaserver::gui
