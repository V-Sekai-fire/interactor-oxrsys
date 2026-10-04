// SPDX-License-Identifier: MPL-2.0
//
// The OXRSys head-mounted display for the PC VR runtime's driver interface: the runtime's
// compositor renders into shared D3D11 textures this driver creates, and each presented frame
// is packed and streamed over PyroWave exactly as the OpenXR runtime streams its swapchains.

#include "openvr_driver_min.h"

#include "Config.h"
#include "ControllerLayout.h"
#include "D3D11Interop.h"
#include "GraphicsTypes.h"
#include "StreamingServer.h"
#include "TrackingReceiver.h"
#include "VulkanDispatch.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace oxrvr;

// D3D11Interop links against the OpenXR entry point's dispatch table; the driver never fills it.
VulkanDispatch gVulkanDispatch;

namespace
{

constexpr uint32_t kEyeWidth = 1512;
constexpr uint32_t kEyeHeight = 1680;
constexpr float kRefreshHz = 90.0f;
constexpr float kIpdMeters = 0.063f;
constexpr char kSettingsSection[] = "driver_oxrsys";
// Our own tracking universe, and its room setup supplied by the driver, which SteamVR prefers over a
// saved one: the poses already put the floor at y = 0, so the standing transform is the identity and
// no calibration saved on the desk can move the floor. The id spells OXRS.
constexpr uint64_t kUniverseId = 0x4F585253;
constexpr char kChaperoneJson[] =
    R"({"jsonid":"chaperone_info","version":5,"universes":[{"universeID":"1331188307",)"
    R"("standing":{"translation":[0,0,0],"yaw":0},"seated":{"translation":[0,0,0],"yaw":0},"play_area":[2,2],)"
    R"("collision_bounds":[[[-1,0,-1],[-1,2.4,-1],[-1,2.4,1],[-1,0,1]],[[-1,0,1],[-1,2.4,1],[1,2.4,1],[1,0,1]],)"
    R"([[1,0,1],[1,2.4,1],[1,2.4,-1],[1,0,-1]],[[1,0,-1],[1,2.4,-1],[-1,2.4,-1],[-1,0,-1]]]}]})";

IVRServerDriverHost* gHost = nullptr;
IVRProperties* gProperties = nullptr;
IVRDriverLog* gLog = nullptr;
IVRSettings* gSettings = nullptr;
IVRDriverInput* gInput = nullptr;

void Log(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer) - 2, format, args);
    va_end(args);
    std::strcat(buffer, "\n");
    if (gLog != nullptr)
        gLog->Log(buffer);
}

// Logs the first call of each implemented slot, which the live-load check counts.
#define OXRSYS_HIT(name)                                                                           \
    do                                                                                             \
    {                                                                                              \
        static std::atomic_bool hit{false};                                                        \
        if (!hit.exchange(true))                                                                   \
            Log("oxrsys: hit %s", name);                                                           \
    } while (false)

template <typename T>
void WriteProperty(PropertyContainerHandle_t container, ETrackedDeviceProperty prop, T value, PropertyTypeTag_t tag)
{
    PropertyWrite_t write = {};
    write.prop = prop;
    write.writeType = PropertyWrite_Set;
    write.pvBuffer = &value;
    write.unBufferSize = sizeof(T);
    write.unTag = tag;
    gProperties->WritePropertyBatch(container, &write, 1);
}

void WriteString(PropertyContainerHandle_t container, ETrackedDeviceProperty prop, const char* value)
{
    PropertyWrite_t write = {};
    write.prop = prop;
    write.writeType = PropertyWrite_Set;
    write.pvBuffer = const_cast<char*>(value);
    write.unBufferSize = static_cast<uint32_t>(std::strlen(value) + 1);
    write.unTag = k_unStringPropertyTag;
    gProperties->WritePropertyBatch(container, &write, 1);
}

HmdQuaternion_t Identity()
{
    HmdQuaternion_t q = {1.0, 0.0, 0.0, 0.0};
    return q;
}

// Rotation part of a row-major 3x4 pose to an xyzw quaternion, position to xyz.
void PoseFromMatrix(const HmdMatrix34_t& m, float orientation[4], float position[3])
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m.m[2][1] - m.m[1][2]) / s;
        y = (m.m[0][2] - m.m[2][0]) / s;
        z = (m.m[1][0] - m.m[0][1]) / s;
    }
    else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        w = (m.m[2][1] - m.m[1][2]) / s;
        x = 0.25f * s;
        y = (m.m[0][1] + m.m[1][0]) / s;
        z = (m.m[0][2] + m.m[2][0]) / s;
    }
    else if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        w = (m.m[0][2] - m.m[2][0]) / s;
        x = (m.m[0][1] + m.m[1][0]) / s;
        y = 0.25f * s;
        z = (m.m[1][2] + m.m[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        w = (m.m[1][0] - m.m[0][1]) / s;
        x = (m.m[0][2] + m.m[2][0]) / s;
        y = (m.m[1][2] + m.m[2][1]) / s;
        z = 0.25f * s;
    }
    orientation[0] = x;
    orientation[1] = y;
    orientation[2] = z;
    orientation[3] = w;
    position[0] = m.m[0][3];
    position[1] = m.m[1][3];
    position[2] = m.m[2][3];
}

DXGI_FORMAT CopyableUnormFormat(uint32_t format)
{
    switch (static_cast<DXGI_FORMAT>(format))
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

struct TextureSet
{
    uint32_t pid = 0;
    std::array<ComPtr<ID3D11Texture2D>, 3> textures;
    std::array<SharedTextureHandle_t, 3> handles = {};
    uint32_t index = 0;
};

// One leased pair of eye copies; the encoder holds the lease until it has packed them.
struct EyePair
{
    std::array<ComPtr<ID3D11Texture2D>, 2> textures;
    std::array<Win32EyeImage, 2> images = {};
    std::atomic_bool leased{false};
};

class DisplayComponent final : public IVRDisplayComponent
{
public:
    void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override
    {
        OXRSYS_HIT("IVRDisplayComponent::GetWindowBounds");
        *pnX = 0;
        *pnY = 0;
        *pnWidth = kEyeWidth * 2;
        *pnHeight = kEyeHeight;
    }
    bool IsDisplayOnDesktop() override
    {
        OXRSYS_HIT("IVRDisplayComponent::IsDisplayOnDesktop");
        return false;
    }
    bool IsDisplayRealDisplay() override
    {
        OXRSYS_HIT("IVRDisplayComponent::IsDisplayRealDisplay");
        return false;
    }
    void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override
    {
        OXRSYS_HIT("IVRDisplayComponent::GetRecommendedRenderTargetSize");
        *pnWidth = kEyeWidth;
        *pnHeight = kEyeHeight;
    }
    void GetEyeOutputViewport(EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth,
                              uint32_t* pnHeight) override
    {
        OXRSYS_HIT("IVRDisplayComponent::GetEyeOutputViewport");
        *pnX = eEye == Eye_Left ? 0 : kEyeWidth;
        *pnY = 0;
        *pnWidth = kEyeWidth;
        *pnHeight = kEyeHeight;
    }
    void GetProjectionRaw(EVREye /*eEye*/, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override
    {
        OXRSYS_HIT("IVRDisplayComponent::GetProjectionRaw");
        *pfLeft = -1.0f;
        *pfRight = 1.0f;
        *pfTop = -1.0f;
        *pfBottom = 1.0f;
    }
    DistortionCoordinates_t ComputeDistortion(EVREye /*eEye*/, float fU, float fV) override
    {
        OXRSYS_HIT("IVRDisplayComponent::ComputeDistortion");
        DistortionCoordinates_t coordinates = {{fU, fV}, {fU, fV}, {fU, fV}};
        return coordinates;
    }
    bool ComputeInverseDistortion(HmdVector2_t* /*pResult*/, EVREye /*eEye*/, uint32_t /*unChannel*/, float /*fU*/,
                                  float /*fV*/) override
    {
        OXRSYS_HIT("IVRDisplayComponent::ComputeInverseDistortion");
        return false;
    }
};

class DirectModeComponent final : public IVRDriverDirectModeComponent
{
public:
    bool Initialize()
    {
        uint8_t luid[8] = {};
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (Win32GetRuntimeAdapterLuid(luid) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc = {};
                adapter->GetDesc1(&desc);
                if (std::memcmp(&desc.AdapterLuid, luid, sizeof(luid)) == 0)
                    break;
                adapter.Reset();
            }
        }
        std::memcpy(&adapterLuid_, luid, sizeof(luid));
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                                       &device, nullptr, &context);
        if (FAILED(hr) || FAILED(device.As(&device_)) || FAILED(context.As(&context_)))
        {
            Log("oxrsys: D3D11 device creation failed (0x%08x)", static_cast<unsigned>(hr));
            return false;
        }
        // The encoder thread uses the same immediate context.
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device_.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);
        return true;
    }

    void SetStreaming(StreamingServer* server) { server_ = server; }
    ID3D11Device5* Device() const { return device_.Get(); }
    uint64_t AdapterLuid() const { return adapterLuid_; }
    void RequestDump(const std::string& path, uint64_t frame)
    {
        dumpPath_ = path;
        dumpFrame_ = frame;
    }
    void SetNextVsync(std::chrono::steady_clock::time_point next) { nextVsyncNs_ = next.time_since_epoch().count(); }

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::CreateSwapTextureSet");
        std::shared_ptr<TextureSet> set = std::make_shared<TextureSet>();
        set->pid = unPid;
        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = desc->nWidth;
        textureDesc.Height = desc->nHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = static_cast<DXGI_FORMAT>(desc->nFormat);
        textureDesc.SampleDesc.Count = desc->nSampleCount > 0 ? desc->nSampleCount : 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        std::scoped_lock lock(mutex_);
        for (size_t i = 0; i < set->textures.size(); ++i)
        {
            ComPtr<IDXGIResource> resource;
            HANDLE handle = nullptr;
            if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, &set->textures[i])) ||
                FAILED(set->textures[i].As(&resource)) || FAILED(resource->GetSharedHandle(&handle)))
            {
                Log("oxrsys: swap texture %ux%u format %u failed", desc->nWidth, desc->nHeight, desc->nFormat);
                std::memset(out, 0, sizeof(*out));
                return;
            }
            set->handles[i] = reinterpret_cast<SharedTextureHandle_t>(handle);
            out->rSharedTextureHandles[i] = set->handles[i];
            sets_[set->handles[i]] = set;
        }
        out->unTextureFlags = 0;
        Log("oxrsys: swap texture set %ux%u format %u samples %u for pid %u", desc->nWidth, desc->nHeight,
            desc->nFormat, desc->nSampleCount, unPid);
    }

    void DestroySwapTextureSet(SharedTextureHandle_t sharedTextureHandle) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::DestroySwapTextureSet");
        std::scoped_lock lock(mutex_);
        std::map<SharedTextureHandle_t, std::shared_ptr<TextureSet>>::iterator it = sets_.find(sharedTextureHandle);
        if (it == sets_.end())
            return;
        std::shared_ptr<TextureSet> set = it->second;
        for (SharedTextureHandle_t handle : set->handles)
            sets_.erase(handle);
    }

    void DestroyAllSwapTextureSets(uint32_t unPid) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::DestroyAllSwapTextureSets");
        std::scoped_lock lock(mutex_);
        for (std::map<SharedTextureHandle_t, std::shared_ptr<TextureSet>>::iterator it = sets_.begin();
             it != sets_.end();)
            it = it->second->pid == unPid ? sets_.erase(it) : std::next(it);
    }

    void GetNextSwapTextureSetIndex(SharedTextureHandle_t* sharedTextureHandles, uint32_t (*pIndices)[2]) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::GetNextSwapTextureSetIndex");
        std::scoped_lock lock(mutex_);
        for (int eye = 0; eye < 2; ++eye)
        {
            std::map<SharedTextureHandle_t, std::shared_ptr<TextureSet>>::iterator it =
                sets_.find(sharedTextureHandles[eye]);
            if (it == sets_.end())
                continue;
            it->second->index = (it->second->index + 1) % 3;
            (*pIndices)[eye] = it->second->index;
        }
    }

    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::SubmitLayer");
        // The first layer is the scene; overlay layers are not composited in this pass.
        if (layerCount_++ == 0)
        {
            layer_[0] = perEye[0];
            layer_[1] = perEye[1];
        }
    }

    void Present(SharedTextureHandle_t syncTexture) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::Present");
        const uint32_t layers = layerCount_;
        layerCount_ = 0;
        if (layers == 0)
            return;

        if (syncTexture != syncHandle_)
        {
            syncHandle_ = syncTexture;
            syncMutex_.Reset();
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(device_->OpenSharedResource(reinterpret_cast<HANDLE>(syncTexture), IID_PPV_ARGS(&texture))) ||
                FAILED(texture.As(&syncMutex_)))
            {
                Log("oxrsys: opening the sync texture failed");
                syncMutex_.Reset();
            }
        }
        if (!syncMutex_ || syncMutex_->AcquireSync(0, 100) != S_OK)
            return;

        std::shared_ptr<EyePair> pair;
        {
            std::scoped_lock lock(mutex_);
            pair = CopyLayer();
        }
        syncMutex_->ReleaseSync(0);
        if (!pair)
            return;
        ++presentedFrames_;

        if (presentedFrames_ % 900 == 1)
        {
            float o[4] = {};
            float p[3] = {};
            PoseFromMatrix(layer_[0].mHmdPose, o, p);
            const double yaw = std::atan2(2.0 * (o[3] * o[1] + o[0] * o[2]), 1.0 - 2.0 * (o[0] * o[0] + o[1] * o[1]));
            Log("oxrsys: presented %llu frames, eye %ux%u, render pose yaw %.1f deg at (%.2f, %.2f, %.2f)",
                static_cast<unsigned long long>(presentedFrames_), pairWidth_, pairHeight_, yaw * 57.29578, p[0], p[1],
                p[2]);
        }
        if (!dumpPath_.empty() && presentedFrames_ >= dumpFrame_)
        {
            Dump(*pair, dumpPath_);
            dumpPath_.clear();
        }

        if (server_ == nullptr || !server_->IsClientConnected())
        {
            pair->leased = false;
            return;
        }
        std::shared_ptr<void> lease(nullptr, [pair](void*) { pair->leased = false; });
        FrameSource frame = {};
        frame.left.api = GraphicsApi::D3D11;
        frame.left.image = std::shared_ptr<void>(pair, &pair->images[0]);
        frame.left.lifetime = lease;
        frame.right.api = GraphicsApi::D3D11;
        frame.right.image = std::shared_ptr<void>(pair, &pair->images[1]);
        frame.right.lifetime = lease;
        float orientation[4] = {};
        float position[3] = {};
        PoseFromMatrix(layer_[0].mHmdPose, orientation, position);
        server_->SendFrame(std::move(frame), orientation, position);
    }

    void PostPresent(const Throttling_t* /*pThrottling*/) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::PostPresent");
        const std::chrono::steady_clock::time_point next{std::chrono::steady_clock::duration(nextVsyncNs_.load())};
        if (next > std::chrono::steady_clock::now())
            std::this_thread::sleep_until(next);
    }

    void GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming) override
    {
        OXRSYS_HIT("IVRDriverDirectModeComponent::GetFrameTiming");
        pFrameTiming->m_nNumFramePresents = 1;
        pFrameTiming->m_nNumMisPresented = 0;
        pFrameTiming->m_nNumDroppedFrames = 0;
        pFrameTiming->m_nReprojectionFlags = 0;
    }

    uint64_t PresentedFrames() const { return presentedFrames_; }

private:
    std::shared_ptr<EyePair> CopyLayer()
    {
        std::array<ID3D11Texture2D*, 2> sources = {};
        std::array<D3D11_BOX, 2> boxes = {};
        D3D11_TEXTURE2D_DESC sourceDesc = {};
        for (int eye = 0; eye < 2; ++eye)
        {
            std::map<SharedTextureHandle_t, std::shared_ptr<TextureSet>>::iterator it = sets_.find(layer_[eye].hTexture);
            if (it == sets_.end())
                return nullptr;
            TextureSet& set = *it->second;
            for (size_t i = 0; i < set.handles.size(); ++i)
                if (set.handles[i] == layer_[eye].hTexture)
                    sources[eye] = set.textures[i].Get();
            sources[eye]->GetDesc(&sourceDesc);
            const VRTextureBounds_t& b = layer_[eye].bounds;
            boxes[eye].left = static_cast<UINT>(std::fmin(b.uMin, b.uMax) * sourceDesc.Width);
            boxes[eye].right = static_cast<UINT>(std::fmax(b.uMin, b.uMax) * sourceDesc.Width);
            boxes[eye].top = static_cast<UINT>(std::fmin(b.vMin, b.vMax) * sourceDesc.Height);
            boxes[eye].bottom = static_cast<UINT>(std::fmax(b.vMin, b.vMax) * sourceDesc.Height);
            boxes[eye].back = 1;
        }
        const DXGI_FORMAT format = CopyableUnormFormat(sourceDesc.Format);
        if (format == DXGI_FORMAT_UNKNOWN || sourceDesc.SampleDesc.Count != 1)
        {
            if (!loggedFormat_.exchange(true))
                Log("oxrsys: format %u x%u samples is not streamed", sourceDesc.Format, sourceDesc.SampleDesc.Count);
            return nullptr;
        }
        const uint32_t width = (boxes[0].right - boxes[0].left) & ~1u;
        const uint32_t height = (boxes[0].bottom - boxes[0].top) & ~1u;
        boxes[0].right = boxes[0].left + width;
        boxes[0].bottom = boxes[0].top + height;
        boxes[1].right = boxes[1].left + width;
        boxes[1].bottom = boxes[1].top + height;

        if (width != pairWidth_ || height != pairHeight_ || format != pairFormat_)
        {
            for (std::shared_ptr<EyePair>& pair : pairs_)
                pair.reset();
            pairWidth_ = width;
            pairHeight_ = height;
            pairFormat_ = format;
        }
        std::shared_ptr<EyePair> pair;
        for (std::shared_ptr<EyePair>& candidate : pairs_)
        {
            if (!candidate)
            {
                candidate = std::make_shared<EyePair>();
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                for (int eye = 0; eye < 2; ++eye)
                {
                    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &candidate->textures[eye])))
                        return nullptr;
                    candidate->images[eye] = {candidate->textures[eye].Get(), width, height};
                }
            }
            bool expected = false;
            if (candidate->leased.compare_exchange_strong(expected, true))
            {
                pair = candidate;
                break;
            }
        }
        if (!pair)
            return nullptr;
        for (int eye = 0; eye < 2; ++eye)
            context_->CopySubresourceRegion(pair->textures[eye].Get(), 0, 0, 0, 0, sources[eye], 0, &boxes[eye]);
        return pair;
    }

    // Both eyes side by side as a 32-bit top-down BMP.
    void Dump(const EyePair& pair, const std::string& path)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        pair.textures[0]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * 2 * desc.Height * 4);
        for (int eye = 0; eye < 2; ++eye)
        {
            ComPtr<ID3D11Texture2D> staging;
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging)))
                return;
            context_->CopyResource(staging.Get(), pair.textures[eye].Get());
            if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return;
            for (uint32_t row = 0; row < desc.Height; ++row)
            {
                const uint8_t* source = static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
                uint8_t* target = pixels.data() + (static_cast<size_t>(row) * desc.Width * 2 + eye * desc.Width) * 4;
                for (uint32_t x = 0; x < desc.Width; ++x)
                {
                    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
                    target[x * 4 + 0] = source[x * 4 + (rgba ? 2 : 0)];
                    target[x * 4 + 1] = source[x * 4 + 1];
                    target[x * 4 + 2] = source[x * 4 + (rgba ? 0 : 2)];
                    target[x * 4 + 3] = source[x * 4 + 3];
                }
            }
            context_->Unmap(staging.Get(), 0);
        }
        const int32_t width = static_cast<int32_t>(desc.Width * 2);
        const int32_t height = -static_cast<int32_t>(desc.Height);
        BITMAPFILEHEADER file = {};
        BITMAPINFOHEADER info = {};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        Log("oxrsys: dumped frame %llu (%dx%d, format %u) to %s", static_cast<unsigned long long>(presentedFrames_),
            width, desc.Height, desc.Format, path.c_str());
    }

    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> context_;
    uint64_t adapterLuid_ = 0;
    std::mutex mutex_;
    std::map<SharedTextureHandle_t, std::shared_ptr<TextureSet>> sets_;
    SubmitLayerPerEye_t layer_[2] = {};
    uint32_t layerCount_ = 0;
    SharedTextureHandle_t syncHandle_ = 0;
    ComPtr<IDXGIKeyedMutex> syncMutex_;
    std::array<std::shared_ptr<EyePair>, 3> pairs_;
    uint32_t pairWidth_ = 0;
    uint32_t pairHeight_ = 0;
    DXGI_FORMAT pairFormat_ = DXGI_FORMAT_UNKNOWN;
    std::atomic_bool loggedFormat_{false};
    std::string dumpPath_;
    uint64_t dumpFrame_ = 0;
    uint64_t presentedFrames_ = 0;
    StreamingServer* server_ = nullptr;
    std::atomic<int64_t> nextVsyncNs_{0};
};

// A hand that is connected while the client reports its controller active; without controllers the
// dashboard pointer is the head and the headset button selects.
class Controller final : public ITrackedDeviceServerDriver
{
public:
    explicit Controller(bool left)
        : left_(left)
        , layout_(LayoutForControllers(Config::Get().GetValues().controllers))
    {
    }

    EVRInitError Activate(uint32_t unObjectId) override
    {
        id_ = unObjectId;
        const PropertyContainerHandle_t c = gProperties->TrackedDeviceToPropertyContainer(unObjectId);
        WriteString(c, Prop_TrackingSystemName_String, "oxrsys");
        WriteString(c, Prop_ModelNumber_String, "OXRSys Controller");
        WriteString(c, Prop_SerialNumber_String, left_ ? "OXRSYS-LEFT-0" : "OXRSYS-RIGHT-0");
        WriteString(c, Prop_ManufacturerName_String, "OXRSys");
        // A profile SteamVR ships, so apps use their own bindings for that controller.
        WriteString(c, Prop_ControllerType_String, layout_.controllerType);
        // Hidden until physical tracker support lands; oxrsys_controller is the model to restore.
        WriteString(c, Prop_RenderModelName_String, "{oxrsys}oxrsys_hidden");
        WriteString(c, Prop_InputProfilePath_String, layout_.inputProfilePath);
        WriteProperty(c, Prop_ControllerRoleHint_Int32,
                      static_cast<int32_t>(left_ ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand),
                      k_unInt32PropertyTag);
        if (gInput != nullptr)
        {
            const std::vector<ControllerInput>& inputs = left_ ? layout_.left : layout_.right;
            handles_.assign(inputs.size(), 0);
            for (size_t i = 0; i < inputs.size(); ++i)
            {
                if (inputs[i].scalar)
                    gInput->CreateScalarComponent(c, inputs[i].path.c_str(), &handles_[i], VRScalarType_Absolute,
                                                  inputs[i].twoSided ? VRScalarUnits_NormalizedTwoSided
                                                                     : VRScalarUnits_NormalizedOneSided);
                else
                    gInput->CreateBooleanComponent(c, inputs[i].path.c_str(), &handles_[i]);
            }
        }
        return VRInitError_None;
    }

    void Deactivate() override { id_ = kInvalidId; }
    void EnterStandby() override {}
    void* GetComponent(const char* /*name*/) override { return nullptr; }
    void DebugRequest(const char* /*pchRequest*/, char* pchResponseBuffer, uint32_t unResponseBufferSize) override
    {
        if (unResponseBufferSize > 0)
            pchResponseBuffer[0] = '\0';
    }
    DriverPose_t GetPose() override { return pose_; }

    void Update(const oxr::protocol::TrackingPacket& packet)
    {
        if (id_ == kInvalidId)
            return;
        const float grip = left_ ? packet.leftGrip : packet.rightGrip;
        const float trigger = left_ ? packet.leftTrigger : packet.rightTrigger;
        const float* position = left_ ? packet.leftControllerPos : packet.rightControllerPos;
        const float* rotation = left_ ? packet.leftControllerRot : packet.rightControllerRot;
        pose_ = {};
        pose_.qWorldFromDriverRotation = Identity();
        pose_.qDriverFromHeadRotation = Identity();
        for (int i = 0; i < 3; ++i)
            pose_.vecPosition[i] = position[i];
        pose_.qRotation.x = rotation[0];
        pose_.qRotation.y = rotation[1];
        pose_.qRotation.z = rotation[2];
        pose_.qRotation.w = rotation[3];
        pose_.result = TrackingResult_Running_OK;
        const bool present = (packet.trackingFlags & (left_ ? oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE
                                                            : oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE)) != 0;
        pose_.poseIsValid = present;
        pose_.deviceIsConnected = present;
        gHost->TrackedDevicePoseUpdated(id_, pose_, sizeof(pose_));
        if (gInput != nullptr)
        {
            const std::vector<ControllerInput>& inputs = left_ ? layout_.left : layout_.right;
            for (size_t i = 0; i < inputs.size() && i < handles_.size(); ++i)
            {
                const float value = ControllerSourceValue(inputs[i].source, packet, left_);
                if (inputs[i].scalar)
                    gInput->UpdateScalarComponent(handles_[i], value, 0.0);
                else
                    gInput->UpdateBooleanComponent(handles_[i], value != 0.0f, 0.0);
            }
        }
    }

private:
    static constexpr uint32_t kInvalidId = 0xFFFFFFFFu;
    bool left_;
    const ControllerLayout& layout_;
    uint32_t id_ = kInvalidId;
    DriverPose_t pose_ = {};
    std::vector<VRInputComponentHandle_t> handles_;
};

class Hmd final : public ITrackedDeviceServerDriver
{
public:
    ~Hmd()
    {
        active_ = false;
        if (tick_.joinable())
            tick_.join();
    }

    EVRInitError Activate(uint32_t unObjectId) override
    {
        OXRSYS_HIT("ITrackedDeviceServerDriver::Activate");
        id_ = unObjectId;
        const PropertyContainerHandle_t c = gProperties->TrackedDeviceToPropertyContainer(unObjectId);
        WriteString(c, Prop_TrackingSystemName_String, "oxrsys");
        WriteString(c, Prop_ModelNumber_String, "OXRSys");
        WriteString(c, Prop_SerialNumber_String, "OXRSYS-HMD-0");
        WriteString(c, Prop_ManufacturerName_String, "OXRSys");
        // Status-window icons from resources/icons, drawn from resources/icons/headset.svg.
        WriteString(c, Prop_NamedIconPathDeviceOff_String, "{oxrsys}/icons/headset_status_off.png");
        WriteString(c, Prop_NamedIconPathDeviceSearching_String, "{oxrsys}/icons/headset_status_searching.png");
        WriteString(c, Prop_NamedIconPathDeviceSearchingAlert_String, "{oxrsys}/icons/headset_status_searching_alert.png");
        WriteString(c, Prop_NamedIconPathDeviceReady_String, "{oxrsys}/icons/headset_status_ready.png");
        WriteString(c, Prop_NamedIconPathDeviceReadyAlert_String, "{oxrsys}/icons/headset_status_ready_alert.png");
        WriteString(c, Prop_NamedIconPathDeviceNotReady_String, "{oxrsys}/icons/headset_status_not_ready.png");
        WriteString(c, Prop_NamedIconPathDeviceStandby_String, "{oxrsys}/icons/headset_status_standby.png");
        WriteString(c, Prop_NamedIconPathDeviceAlertLow_String, "{oxrsys}/icons/headset_status_alert_low.png");
        WriteString(c, Prop_NamedIconPathDeviceStandbyAlert_String, "{oxrsys}/icons/headset_status_standby_alert.png");
        WriteProperty(c, Prop_DisplayFrequency_Float, kRefreshHz, k_unFloatPropertyTag);
        WriteProperty(c, Prop_UserIpdMeters_Float, kIpdMeters, k_unFloatPropertyTag);
        WriteProperty(c, Prop_SecondsFromVsyncToPhotons_Float, 0.0f, k_unFloatPropertyTag);
        WriteProperty(c, Prop_DriverDirectModeSendsVsyncEvents_Bool, true, k_unBoolPropertyTag);
        WriteProperty(c, Prop_IsOnDesktop_Bool, false, k_unBoolPropertyTag);
        WriteProperty(c, Prop_CurrentUniverseId_Uint64, kUniverseId, k_unUint64PropertyTag);
        WriteString(c, Prop_DriverProvidedChaperoneJson_String, kChaperoneJson);
        WriteProperty(c, Prop_GraphicsAdapterLuid_Uint64, direct_.AdapterLuid(), k_unUint64PropertyTag);
        // The headset's button, as on a headset whose button selects by gaze when no controller is held.
        WriteString(c, Prop_ControllerType_String, "oxrsys_hmd");
        WriteString(c, Prop_InputProfilePath_String, "{oxrsys}/input/oxrsys_hmd_profile.json");
        if (gInput != nullptr)
            gInput->CreateBooleanComponent(c, "/input/system/click", &systemClick_);
        active_ = true;
        tick_ = std::thread([this]() { TickThread(); });
        return VRInitError_None;
    }

    void Deactivate() override
    {
        OXRSYS_HIT("ITrackedDeviceServerDriver::Deactivate");
        active_ = false;
        if (tick_.joinable())
            tick_.join();
    }

    void EnterStandby() override { OXRSYS_HIT("ITrackedDeviceServerDriver::EnterStandby"); }

    void* GetComponent(const char* name) override
    {
        OXRSYS_HIT("ITrackedDeviceServerDriver::GetComponent");
        if (std::strcmp(name, kIVRDisplayComponent_Version) == 0)
        {
            OXRSYS_HIT("GetComponent(IVRDisplayComponent)");
            return static_cast<IVRDisplayComponent*>(&display_);
        }
        if (std::strcmp(name, kIVRDriverDirectModeComponent_Version) == 0)
        {
            OXRSYS_HIT("GetComponent(IVRDriverDirectModeComponent)");
            return static_cast<IVRDriverDirectModeComponent*>(&direct_);
        }
        return nullptr;
    }

    void DebugRequest(const char* /*pchRequest*/, char* pchResponseBuffer, uint32_t unResponseBufferSize) override
    {
        OXRSYS_HIT("ITrackedDeviceServerDriver::DebugRequest");
        if (unResponseBufferSize > 0)
            pchResponseBuffer[0] = '\0';
    }

    DriverPose_t GetPose() override
    {
        OXRSYS_HIT("ITrackedDeviceServerDriver::GetPose");
        return CurrentPose();
    }

    DirectModeComponent& Direct() { return direct_; }
    void SetHands(Controller* left, Controller* right) { hands_ = {left, right}; }
    void SetStreaming(StreamingServer* server)
    {
        server_ = server;
        direct_.SetStreaming(server);
    }

private:
    DriverPose_t CurrentPose(oxr::protocol::TrackingPacket* latest = nullptr)
    {
        DriverPose_t pose = {};
        pose.qWorldFromDriverRotation = Identity();
        pose.qDriverFromHeadRotation = Identity();
        pose.qRotation = Identity();
        pose.vecPosition[1] = 1.6;
        TrackingReceiver* receiver = server_ != nullptr ? server_->GetTrackingReceiver() : nullptr;
        oxr::protocol::TrackingPacket packet = {};
        if (receiver != nullptr && (receiver->GetPredictedPose(packet) || receiver->GetLatestPose(packet)))
        {
            for (int i = 0; i < 3; ++i)
                pose.vecPosition[i] = packet.headPosition[i];
            pose.qRotation.x = packet.headOrientation[0];
            pose.qRotation.y = packet.headOrientation[1];
            pose.qRotation.z = packet.headOrientation[2];
            pose.qRotation.w = packet.headOrientation[3];
            if (latest != nullptr)
                *latest = packet;
        }
        pose.result = TrackingResult_Running_OK;
        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        return pose;
    }

    // Publishes the head pose and the vsync the compositor paces on, once per refresh.
    void TickThread()
    {
        const std::chrono::nanoseconds period(static_cast<int64_t>(1e9 / kRefreshHz));
        std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
        while (active_)
        {
            next += period;
            direct_.SetNextVsync(next);
            oxr::protocol::TrackingPacket packet = {};
            const DriverPose_t pose = CurrentPose(&packet);
            gHost->TrackedDevicePoseUpdated(id_, pose, sizeof(pose));
            gHost->VsyncEvent(0.0);
            if (gInput != nullptr)
                gInput->UpdateBooleanComponent(systemClick_, (packet.buttonState & oxr::protocol::BUTTON_HEADSET_SYSTEM) != 0, 0.0);
            for (Controller* hand : hands_)
                if (hand != nullptr)
                    hand->Update(packet);
            std::this_thread::sleep_until(next);
        }
    }

    uint32_t id_ = 0;
    VRInputComponentHandle_t systemClick_ = 0;
    std::array<Controller*, 2> hands_ = {nullptr, nullptr};
    std::atomic_bool active_{false};
    std::thread tick_;
    StreamingServer* server_ = nullptr;
    DisplayComponent display_;
    DirectModeComponent direct_;
};

class Provider final : public IServerTrackedDeviceProvider
{
public:
    EVRInitError Init(IVRDriverContext* context) override
    {
        OXRSYS_HIT("IServerTrackedDeviceProvider::Init");
        gHost = static_cast<IVRServerDriverHost*>(context->GetGenericInterface(kIVRServerDriverHost_Version, nullptr));
        gProperties = static_cast<IVRProperties*>(context->GetGenericInterface(kIVRProperties_Version, nullptr));
        gLog = static_cast<IVRDriverLog*>(context->GetGenericInterface(kIVRDriverLog_Version, nullptr));
        gSettings = static_cast<IVRSettings*>(context->GetGenericInterface(kIVRSettings_Version, nullptr));
        gInput = static_cast<IVRDriverInput*>(context->GetGenericInterface(kIVRDriverInput_Version, nullptr));
        if (gHost == nullptr || gProperties == nullptr || gLog == nullptr || gSettings == nullptr)
            return VRInitError_Init_InterfaceNotFound;

        hmd_ = std::make_unique<Hmd>();
        if (!hmd_->Direct().Initialize())
            return VRInitError_Init_InterfaceNotFound;

        char dumpPath[MAX_PATH] = {};
        EVRSettingsError settingsError = VRSettingsError_None;
        gSettings->GetString(kSettingsSection, "frameDumpPath", dumpPath, sizeof(dumpPath), &settingsError);
        if (settingsError == VRSettingsError_None && dumpPath[0] != '\0')
        {
            const int32_t frame = gSettings->GetInt32(kSettingsSection, "frameDumpFrame", &settingsError);
            hmd_->Direct().RequestDump(dumpPath, settingsError == VRSettingsError_None && frame > 0 ? frame : 1);
        }

        D3D11GraphicsContext d3d11 = {};
        d3d11.device = hmd_->Direct().Device();
        d3d11.encoderDevice = true;
        server_ = std::make_unique<StreamingServer>();
        server_->SetGraphicsContext(GraphicsContext::D3D11(d3d11));
        // The compositor renders with GetProjectionRaw's tangents and this IPD, so the hologram does too.
        const float tangents[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        server_->SetRenderEyes(tangents, kIpdMeters);
        if (server_->Start(kEyeWidth, kEyeHeight, static_cast<uint32_t>(kRefreshHz)))
        {
            hmd_->SetStreaming(server_.get());
            Log("oxrsys: streaming server started");
        }
        else
        {
            Log("oxrsys: streaming server failed to start");
            server_.reset();
        }

        if (!gHost->TrackedDeviceAdded("OXRSYS-HMD-0", TrackedDeviceClass_HMD, hmd_.get()))
            Log("oxrsys: the head-mounted display was not added");
        left_ = std::make_unique<Controller>(true);
        right_ = std::make_unique<Controller>(false);
        if (gInput == nullptr || !gHost->TrackedDeviceAdded("OXRSYS-LEFT-0", TrackedDeviceClass_Controller, left_.get()) ||
            !gHost->TrackedDeviceAdded("OXRSYS-RIGHT-0", TrackedDeviceClass_Controller, right_.get()))
            Log("oxrsys: the controllers were not added");
        hmd_->SetHands(left_.get(), right_.get());
        Log("oxrsys: provider initialised");
        return VRInitError_None;
    }

    void Cleanup() override
    {
        OXRSYS_HIT("IServerTrackedDeviceProvider::Cleanup");
        if (hmd_)
        {
            hmd_->SetStreaming(nullptr);
            hmd_->SetHands(nullptr, nullptr);
        }
        if (server_)
            server_->Stop();
        server_.reset();
        gHost = nullptr;
    }

    const char* const* GetInterfaceVersions() override
    {
        OXRSYS_HIT("IServerTrackedDeviceProvider::GetInterfaceVersions");
        static const char* const versions[] = {
            kIVRSettings_Version,
            kITrackedDeviceServerDriver_Version,
            kIVRDisplayComponent_Version,
            kIVRDriverDirectModeComponent_Version,
            kIVRDriverInput_Version,
            kIServerTrackedDeviceProvider_Version,
            nullptr,
        };
        return versions;
    }

    void RunFrame() override
    {
        OXRSYS_HIT("IServerTrackedDeviceProvider::RunFrame");
        VREvent_t event = {};
        while (gHost != nullptr && gHost->PollNextEvent(&event, sizeof(event)))
        {
            if (event.eventType == VREvent_Quit)
                Log("oxrsys: quit requested");
        }
    }

    bool ShouldBlockStandbyMode() override
    {
        OXRSYS_HIT("IServerTrackedDeviceProvider::ShouldBlockStandbyMode");
        return false;
    }
    void EnterStandby() override { OXRSYS_HIT("IServerTrackedDeviceProvider::EnterStandby"); }
    void LeaveStandby() override { OXRSYS_HIT("IServerTrackedDeviceProvider::LeaveStandby"); }

private:
    std::unique_ptr<Hmd> hmd_;
    std::unique_ptr<Controller> left_;
    std::unique_ptr<Controller> right_;
    std::unique_ptr<StreamingServer> server_;
};

Provider gProvider;

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode)
{
    if (std::strcmp(interfaceName, kIServerTrackedDeviceProvider_Version) == 0)
        return static_cast<IServerTrackedDeviceProvider*>(&gProvider);
    if (returnCode != nullptr)
        *returnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
