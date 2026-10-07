#include <napi.h>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;

struct D3DResources {
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

D3DResources g_resources;
std::mutex g_resourcesMutex;

std::string HresultText(const char* stage, HRESULT result) {
    char message[160] = {};
    std::snprintf(message, sizeof(message), "%s failed with HRESULT 0x%08lX", stage,
                  static_cast<unsigned long>(result));
    return message;
}

HRESULT EnsureD3DDevice() {
    if (g_resources.device != nullptr && g_resources.context != nullptr) {
        return S_OK;
    }

    constexpr std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_10_0;
    ComPtr<ID3D11Device> device;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &device,
        &selectedLevel,
        &g_resources.context);
    if (result == E_INVALIDARG) {
        constexpr std::array<D3D_FEATURE_LEVEL, 3> compatibleLevels = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            compatibleLevels.data(),
            static_cast<UINT>(compatibleLevels.size()),
            D3D11_SDK_VERSION,
            &device,
            &selectedLevel,
            &g_resources.context);
    }
    if (FAILED(result)) {
        g_resources.context.Reset();
        return result;
    }
    result = device.As(&g_resources.device);
    if (FAILED(result)) {
        g_resources.context.Reset();
        return result;
    }
    return S_OK;
}

class ReadbackWorker final : public Napi::AsyncWorker {
public:
    ReadbackWorker(Napi::Env env, uintptr_t sharedHandle, UINT expectedWidth, UINT expectedHeight)
        : Napi::AsyncWorker(env),
          m_deferred(Napi::Promise::Deferred::New(env)),
          m_sharedHandle(sharedHandle),
          m_expectedWidth(expectedWidth),
          m_expectedHeight(expectedHeight) {}

    Napi::Promise Promise() const {
        return m_deferred.Promise();
    }

    void Execute() override {
        const auto start = std::chrono::steady_clock::now();
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitializeCom = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            m_error = HresultText("CoInitializeEx", comResult);
            SetError(m_error);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_resourcesMutex);
            HRESULT result = EnsureD3DDevice();
            if (FAILED(result)) {
                m_error = HresultText("D3D11CreateDevice(HARDWARE)", result);
            } else {
                result = ReadSharedTexture();
                if (FAILED(result)) {
                    if (m_error.empty()) m_error = HresultText("D3D11 shared-texture readback", result);
                }
            }
        }

        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        const auto finish = std::chrono::steady_clock::now();
        m_readbackMilliseconds = std::chrono::duration<double, std::milli>(finish - start).count();
        if (!m_error.empty()) SetError(m_error);
    }

    void OnOK() override {
        Napi::Object result = Napi::Object::New(Env());
        char hashText[24] = {};
        std::snprintf(hashText, sizeof(hashText), "%016llX",
                      static_cast<unsigned long long>(m_pixelHash));
        result.Set("width", Napi::Number::New(Env(), m_actualWidth));
        result.Set("height", Napi::Number::New(Env(), m_actualHeight));
        result.Set("pixelHash", Napi::String::New(Env(), hashText));
        result.Set("readbackMs", Napi::Number::New(Env(), m_readbackMilliseconds));
        m_deferred.Resolve(result);
        delete this;
    }

    void OnError(const Napi::Error& error) override {
        m_deferred.Reject(error.Value());
        delete this;
    }

private:
    HRESULT ReadSharedTexture() {
        if (m_sharedHandle == 0) {
            m_error = "Electron returned a null shared texture handle";
            return E_HANDLE;
        }

        const HANDLE handle = reinterpret_cast<HANDLE>(m_sharedHandle);
        ComPtr<ID3D11Texture2D> source;
        HRESULT result = g_resources.device->OpenSharedResource1(handle, IID_PPV_ARGS(&source));
        if (FAILED(result)) {
            m_error = HresultText("ID3D11Device1::OpenSharedResource1", result);
            return result;
        }

        D3D11_TEXTURE2D_DESC description = {};
        source->GetDesc(&description);
        if (description.Width != m_expectedWidth || description.Height != m_expectedHeight ||
            description.Width < 320 || description.Height < 180 ||
            description.Width > 1920 || description.Height > 1080) {
            m_error = "Shared texture dimensions changed or are outside 320x180..1920x1080";
            return E_INVALIDARG;
        }
        if (description.SampleDesc.Count != 1 ||
            (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
             description.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
             description.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            m_error = "Shared texture pixel format or sample count is unsupported";
            return DXGI_ERROR_UNSUPPORTED;
        }

        if (g_resources.staging == nullptr ||
            g_resources.width != description.Width ||
            g_resources.height != description.Height ||
            g_resources.format != description.Format) {
            g_resources.staging.Reset();
            D3D11_TEXTURE2D_DESC stagingDescription = description;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDescription.MiscFlags = 0;
            result = g_resources.device->CreateTexture2D(
                &stagingDescription, nullptr, &g_resources.staging);
            if (FAILED(result)) {
                m_error = HresultText("ID3D11Device::CreateTexture2D(STAGING)", result);
                return result;
            }
            g_resources.width = description.Width;
            g_resources.height = description.Height;
            g_resources.format = description.Format;
        }

        g_resources.context->CopyResource(g_resources.staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        result = g_resources.context->Map(
            g_resources.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) {
            m_error = HresultText("ID3D11DeviceContext::Map(STAGING)", result);
            return result;
        }

        // Sample a grid rather than hashing the entire 1080p frame; this detects
        // changing content while keeping the diagnostic readback bounded.
        uint64_t hash = 14695981039346656037ULL;
        const UINT stepX = std::max<UINT>(1, description.Width / 128);
        const UINT stepY = std::max<UINT>(1, description.Height / 72);
        for (UINT y = stepY / 2; y < description.Height; y += stepY) {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                              static_cast<size_t>(y) * mapped.RowPitch;
            for (UINT x = stepX / 2; x < description.Width; x += stepX) {
                const auto* pixel = row + static_cast<size_t>(x) * 4;
                for (size_t channel = 0; channel < 4; ++channel) {
                    hash ^= pixel[channel];
                    hash *= 1099511628211ULL;
                }
            }
        }
        g_resources.context->Unmap(g_resources.staging.Get(), 0);

        m_actualWidth = description.Width;
        m_actualHeight = description.Height;
        m_pixelHash = hash;
        return S_OK;
    }

    Napi::Promise::Deferred m_deferred;
    uintptr_t m_sharedHandle;
    UINT m_expectedWidth;
    UINT m_expectedHeight;
    UINT m_actualWidth = 0;
    UINT m_actualHeight = 0;
    uint64_t m_pixelHash = 0;
    double m_readbackMilliseconds = 0.0;
    std::string m_error;
};

Napi::Value InspectSharedTexture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() != 3 || !info[0].IsBuffer() || !info[1].IsNumber() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Expected (ntHandleBuffer, width, height)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const auto handleBuffer = info[0].As<Napi::Buffer<uint8_t>>();
    if (handleBuffer.Length() != sizeof(uintptr_t)) {
        Napi::TypeError::New(env, "Expected an 8-byte Windows NT HANDLE buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const double widthValue = info[1].As<Napi::Number>().DoubleValue();
    const double heightValue = info[2].As<Napi::Number>().DoubleValue();
    if (widthValue < 1 || widthValue > 1920 || heightValue < 1 || heightValue > 1080 ||
        std::floor(widthValue) != widthValue || std::floor(heightValue) != heightValue) {
        Napi::RangeError::New(env, "Invalid shared-texture dimensions").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    uintptr_t rawHandle = 0;
    std::memcpy(&rawHandle, handleBuffer.Data(), sizeof(rawHandle));
    auto* worker = new ReadbackWorker(
        env,
        rawHandle,
        static_cast<UINT>(widthValue),
        static_cast<UINT>(heightValue));
    Napi::Promise promise = worker->Promise();
    worker->Queue();
    return promise;
}

Napi::Object Initialize(Napi::Env env, Napi::Object exports) {
    exports.Set("inspectSharedTexture", Napi::Function::New(env, InspectSharedTexture));
    return exports;
}

} // namespace

NODE_API_MODULE(gpu_texture_probe, Initialize)
