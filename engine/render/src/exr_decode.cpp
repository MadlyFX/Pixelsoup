#include <mediaserver/render/image_decode.hpp>

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfChromaticitiesAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <Imath/half.h>
#include <OpenColorIO/OpenColorIO.h>

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mediaserver::render {
namespace {
namespace exr = OPENEXR_IMF_NAMESPACE;
namespace ocio = OCIO_NAMESPACE;
using Half = IMATH_NAMESPACE::half;
constexpr std::size_t max_bytes = 512U * 1024U * 1024U;
constexpr std::uint64_t max_pixels = 64U * 1024U * 1024U;

class MemoryStream final : public exr::IStream {
public:
    explicit MemoryStream(std::span<const core::u8> bytes)
        : exr::IStream("memory.exr"), bytes_(bytes) {}
    bool read(char* into, int count) override {
        if (count < 0 || static_cast<std::size_t>(count) > bytes_.size() - position_) {
            failed = true;
            throw std::runtime_error("Truncated EXR");
        }
        std::memcpy(into, bytes_.data() + position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return position_ < bytes_.size();
    }
    std::uint64_t tellg() override { return position_; }
    void seekg(std::uint64_t position) override {
        if (position > bytes_.size()) { failed = true; throw std::runtime_error("EXR seek out of range"); }
        position_ = static_cast<std::size_t>(position);
    }
    bool failed{};
private:
    std::span<const core::u8> bytes_;
    std::size_t position_{};
};

bool close(const IMATH_NAMESPACE::V2f& a, const IMATH_NAMESPACE::V2f& b) {
    return std::abs(a.x - b.x) < 0.0001F && std::abs(a.y - b.y) < 0.0001F;
}
bool matches(const exr::Chromaticities& a, const exr::Chromaticities& b) {
    return close(a.red,b.red) && close(a.green,b.green) && close(a.blue,b.blue)
        && close(a.white,b.white);
}
}

bool exr_bytes(std::span<const core::u8> bytes) noexcept {
    return bytes.size() >= 4U && bytes[0] == 0x76U && bytes[1] == 0x2fU
        && bytes[2] == 0x31U && bytes[3] == 0x01U;
}

DecodedFrame decode_exr_bytes(std::span<const core::u8> bytes) noexcept {
    if (!exr_bytes(bytes) || bytes.size() < 8U || bytes.size() > max_bytes) return {};
    // Deep and multipart headers must not be read through the flat-file API.
    if ((bytes[5] & 0x18U) != 0U) return {};
    try {
        MemoryStream stream{bytes};
        exr::InputFile file{stream, 1};
        const auto& header = file.header();
        if (const auto* aces = header.findTypedAttribute<exr::IntAttribute>("acesImageContainerFlag");
            aces != nullptr && aces->value() != 0) return {}; // AP0 containers are outside the AP1/709 input contract.
        const auto window = header.dataWindow();
        const auto width = static_cast<std::int64_t>(window.max.x) - window.min.x + 1;
        const auto height = static_cast<std::int64_t>(window.max.y) - window.min.y + 1;
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768
            || static_cast<std::uint64_t>(width * height) > max_pixels) return {};
        DecodedFrame result{};
        result.width = static_cast<core::u32>(width);
        result.height = static_cast<core::u32>(height);
        result.format = rhi::TextureFormat::Rgba16Float;
        result.layout = DecodedPixelLayout::Rgba16FloatLinear;
        const exr::Chromaticities ap1{{0.713F,0.293F},{0.165F,0.830F},
            {0.128F,0.044F},{0.32168F,0.33767F}};
        bool chromaticities_present = false;
        if (const auto* attr = header.findTypedAttribute<exr::ChromaticitiesAttribute>("chromaticities")) {
            chromaticities_present = true;
            if (matches(attr->value(), ap1)) result.primaries = ImagePrimaries::AcesCg;
            else if (!matches(attr->value(), exr::Chromaticities{})) return {};
        }
        for (const char* key : {"ocio:colorspace", "oiio:ColorSpace", "colorspace"}) {
            if (const auto* tag = header.findTypedAttribute<exr::StringAttribute>(key)) {
                const auto& name = tag->value();
                const bool aces = name == "ACEScg" || name == "acescg" || name == "lin_ap1";
                const bool rec709 = name == "Linear Rec.709 (sRGB)" || name == "lin_rec709";
                if ((!aces && !rec709) || (chromaticities_present
                    && aces != (result.primaries == ImagePrimaries::AcesCg))) return {};
                result.primaries = aces ? ImagePrimaries::AcesCg : ImagePrimaries::Rec709;
                chromaticities_present = true;
            }
        }
        result.pixels.resize(static_cast<std::size_t>(width * height) * 8U);
        exr::FrameBuffer buffer;
        constexpr std::array names{"R", "G", "B", "A"};
        for (std::size_t channel = 0; channel < names.size(); ++channel) {
            const auto* source = header.channels().findChannel(names[channel]);
            if ((source == nullptr && channel < 3U) || (source != nullptr
                && ((source->type != exr::HALF && source->type != exr::FLOAT)
                    || source->xSampling != 1 || source->ySampling != 1))) return {};
            buffer.insert(names[channel], exr::Slice::Make(exr::HALF,
                result.pixels.data() + channel * 2U, window, 8U,
                static_cast<std::size_t>(width) * 8U, 1, 1, channel == 3U ? 1.0 : 0.0));
        }
        file.setFrameBuffer(buffer);
        file.readPixels(window.min.y, window.max.y);
        if (stream.failed || !file.isComplete()) return {};
        return result;
    } catch (...) { return {}; }
}

bool convert_image_to_working_space(DecodedFrame& frame) noexcept {
    if (frame.primaries == ImagePrimaries::Rec709) return true;
    if (frame.layout != DecodedPixelLayout::Rgba16FloatLinear
        || frame.pixels.size() != static_cast<std::size_t>(frame.width) * frame.height * 8U) return false;
    try {
        const auto config = ocio::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
        const auto processor = config->getProcessor("ACEScg", "Linear Rec.709 (sRGB)")
            ->getOptimizedCPUProcessor(ocio::OPTIMIZATION_NONE);
        for (std::size_t offset = 0; offset < frame.pixels.size(); offset += 8U) {
            std::array<Half, 4> pixel;
            std::memcpy(pixel.data(), frame.pixels.data() + offset, 8U);
            float rgb[3]{static_cast<float>(pixel[0]), static_cast<float>(pixel[1]), static_cast<float>(pixel[2])};
            processor->applyRGB(rgb);
            for (std::size_t i = 0; i < 3U; ++i) pixel[i] = Half{rgb[i]};
            std::memcpy(frame.pixels.data() + offset, pixel.data(), 8U);
        }
        frame.primaries = ImagePrimaries::Rec709;
        return true;
    } catch (...) { return false; }
}

void unpremultiply_exr_image(DecodedFrame& frame) noexcept {
    if (frame.layout != DecodedPixelLayout::Rgba16FloatLinear || frame.pixels.size() % 8U != 0U) return;
    for (std::size_t offset = 0; offset < frame.pixels.size(); offset += 8U) {
        std::array<Half,4> pixel; std::memcpy(pixel.data(), frame.pixels.data() + offset, 8U);
        const float alpha = static_cast<float>(pixel[3]);
        if (alpha == 1.0F) continue;
        for (std::size_t i=0; i<3U; ++i)
            pixel[i] = Half{alpha > 0.0F ? static_cast<float>(pixel[i]) / alpha : 0.0F};
        std::memcpy(frame.pixels.data() + offset, pixel.data(), 8U);
    }
}
}
