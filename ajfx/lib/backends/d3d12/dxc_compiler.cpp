#include <ajfx/d3d12.hpp>

// dxcapi.h needs the COM and OLE declarations a lean windows.h leaves out.
#define NOMINMAX
#include <windows.h>

#include <dxcapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace ajfx::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring widen(std::string_view text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string hresult(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

class Compiler final : public gpu::ShaderCompiler {
public:
    static Result<std::unique_ptr<gpu::ShaderCompiler>> create() {
        // dxil.dll validates and signs; without it D3D12 refuses the bytecode.
        HMODULE validator = LoadLibraryW(L"dxil.dll");
        HMODULE module = LoadLibraryW(L"dxcompiler.dll");
        if (module == nullptr || validator == nullptr) {
            if (module != nullptr) {
                FreeLibrary(module);
            }
            if (validator != nullptr) {
                FreeLibrary(validator);
            }
            return Error{ErrorCode::GpuUnavailable,
                         "dxcompiler.dll and dxil.dll must be next to the executable or on the DLL search path"};
        }
        auto compiler = std::unique_ptr<Compiler>(new Compiler(module, validator));
        const auto create_instance =
            reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(module, "DxcCreateInstance")));
        HRESULT hr = create_instance == nullptr ? E_NOINTERFACE : S_OK;
        if (SUCCEEDED(hr)) {
            hr = create_instance(CLSID_DxcUtils, IID_PPV_ARGS(&compiler->utils_));
        }
        if (SUCCEEDED(hr)) {
            hr = create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler->compiler_));
        }
        if (FAILED(hr)) {
            return Error{ErrorCode::GpuUnavailable, "loading DXC failed (" + hresult(hr) + ")"};
        }
        return std::unique_ptr<gpu::ShaderCompiler>{std::move(compiler)};
    }

    ~Compiler() override {
        compiler_.Reset();
        utils_.Reset();
        FreeLibrary(module_);
        FreeLibrary(validator_);
    }

    [[nodiscard]] gpu::ShaderFormat format() const noexcept override { return gpu::ShaderFormat::Dxil; }

    Result<std::vector<std::uint8_t>> compile(std::string_view source, std::string_view entry_point,
                                              gpu::ShaderStage stage, std::string_view name) override {
        const std::wstring entry = widen(entry_point);
        const std::wstring file = widen(name);
        const wchar_t* profile = stage == gpu::ShaderStage::Vertex ? L"vs_6_2"
                                 : stage == gpu::ShaderStage::Pixel ? L"ps_6_2"
                                                                     : L"cs_6_2";
        // IEEE-strict arithmetic with denormals preserved: no contraction
        // into fused multiply-adds, so results follow the source's order.
        const wchar_t* arguments[] = {file.c_str(), L"-E", entry.c_str(), L"-T", profile, L"-HV", L"2021",
                                      L"-Gis",      L"-denorm", L"preserve", L"-WX", L"-O3", L"-Qstrip_debug",
                                      L"-Qstrip_reflect"};
        const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        HRESULT hr = compiler_->Compile(&buffer, arguments, static_cast<UINT32>(std::size(arguments)), nullptr,
                                        IID_PPV_ARGS(&result));
        HRESULT status = E_FAIL;
        if (SUCCEEDED(hr)) {
            hr = result->GetStatus(&status);
        }
        if (FAILED(hr) || FAILED(status)) {
            std::string message = "compiling '" + std::string{name} + "' failed";
            ComPtr<IDxcBlobUtf8> errors;
            if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors &&
                errors->GetStringLength() > 0U) {
                message += ":\n" + std::string{errors->GetStringPointer(), errors->GetStringLength()};
            }
            return Error{ErrorCode::ShaderCompilation, message};
        }
        ComPtr<IDxcBlob> object;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object) {
            return Error{ErrorCode::ShaderCompilation, "compiling '" + std::string{name} + "' produced no bytecode"};
        }
        const auto* bytes = static_cast<const std::uint8_t*>(object->GetBufferPointer());
        return std::vector<std::uint8_t>(bytes, bytes + object->GetBufferSize());
    }

private:
    Compiler(HMODULE module, HMODULE validator) : module_{module}, validator_{validator} {}

    HMODULE module_;
    HMODULE validator_;
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
};

} // namespace

Result<std::unique_ptr<gpu::ShaderCompiler>> create_shader_compiler() {
    return Compiler::create();
}

} // namespace ajfx::d3d12
