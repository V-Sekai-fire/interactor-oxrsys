// SPDX-License-Identifier: MPL-2.0
//
// Windows streaming encoder: the eyes the D3D11 interop copies out of the swapchain are packed
// side by side into one shared texture, which PyroWave imports on its own Vulkan device on the
// same adapter and encodes after a shared fence, so the frame never leaves the GPU.

#include "VideoEncoder.h"
#include "Config.h"
#include "D3D11Interop.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr size_t PacketBoundary = 64 * 1024;

struct Win32PyroWaveState
{
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Fence> fence;
    uint64_t fenceValue = 0;

    ComPtr<ID3D11Texture2D> packed; // both eyes side by side, shared with PyroWave
    DXGI_FORMAT packedFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t packedWidth = 0;
    uint32_t packedHeight = 0;

    pyrowave_device device_ = nullptr;
    pyrowave_encoder encoder = nullptr;
    pyrowave_sync_object sync = nullptr;
    pyrowave_image image = nullptr;
    pyrowave_image_view view = {};

    std::vector<pyrowave_packet> packets;
    std::vector<uint8_t> bitstream;

    ~Win32PyroWaveState()
    {
        if (image != nullptr)
            pyrowave_image_destroy(image);
        if (sync != nullptr)
            pyrowave_sync_object_destroy(sync);
        if (encoder != nullptr)
            pyrowave_encoder_destroy(encoder);
        if (device_ != nullptr)
            pyrowave_device_destroy(device_);
    }

    // (Re)creates the packed texture and its PyroWave import when the eye size or format changes.
    bool EnsurePacked(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        if (packed && format == packedFormat && width == packedWidth && height == packedHeight)
            return true;
        if (image != nullptr)
        {
            pyrowave_image_destroy(image);
            image = nullptr;
        }
        packed.Reset();

        VkFormat vkFormat = format == DXGI_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_UNORM
                            : format == DXGI_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                                                                  : VK_FORMAT_UNDEFINED;
        if (vkFormat == VK_FORMAT_UNDEFINED)
            return false;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &packed)))
        {
            spdlog::error("PyroWave: packed eye texture creation failed");
            return false;
        }
        ComPtr<IDXGIResource> resource;
        HANDLE handle = nullptr;
        if (FAILED(packed.As(&resource)) || FAILED(resource->GetSharedHandle(&handle)) || handle == nullptr)
        {
            spdlog::error("PyroWave: sharing the packed eye texture failed");
            return false;
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = vkFormat;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        pyrowave_image_create_info info = {};
        info.device = device_;
        info.external_handle = reinterpret_cast<pyrowave_os_handle>(handle);
        info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        info.image_create_info = &imageInfo;
        if (pyrowave_image_create(&info, &image) != PYROWAVE_SUCCESS ||
            pyrowave_image_get_image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &view) !=
                PYROWAVE_SUCCESS)
        {
            spdlog::error("PyroWave: importing the packed eye texture failed");
            return false;
        }
        packedFormat = format;
        packedWidth = width;
        packedHeight = height;
        return true;
    }
};

Win32PyroWaveState* State(void* opaque)
{
    return static_cast<Win32PyroWaveState*>(opaque);
}

const Win32EyeImage* Eye(const FrameImageSource& source)
{
    return source.IsValid() ? static_cast<const Win32EyeImage*>(source.GetImage()) : nullptr;
}

} // namespace

VideoEncoder::VideoEncoder() = default;

oxr::protocol::VideoCodec VideoEncoder::StreamCodec()
{
    return oxr::protocol::VideoCodec::PyroWave;
}

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                              uint32_t bitrateMbps, const GraphicsContext& graphicsContext)
{
    Shutdown();

    // 4:2:0 and the side-by-side split both want even sizes.
    width_ = width & ~3u;
    height_ = height & ~1u;
    eyeWidth_ = width_ / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    if (graphicsContext.api != GraphicsApi::Vulkan && graphicsContext.api != GraphicsApi::D3D11)
    {
        return false;
    }

    auto state = std::make_unique<Win32PyroWaveState>();
    const bool ownDevice = graphicsContext.api == GraphicsApi::D3D11 && graphicsContext.d3d11.encoderDevice;
    ID3D11Device* baseDevice = static_cast<ID3D11Device*>(
        ownDevice ? graphicsContext.d3d11.device : Win32InteropD3D11Device(graphicsContext));
    ComPtr<ID3D11DeviceContext> baseContext;
    if (baseDevice == nullptr || FAILED(baseDevice->QueryInterface(IID_PPV_ARGS(&state->device))))
    {
        spdlog::error("PyroWave: no runtime D3D11 device");
        return false;
    }
    state->device->GetImmediateContext(&baseContext);
    HANDLE fenceHandle = nullptr;
    if (FAILED(baseContext.As(&state->context)) ||
        FAILED(state->device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->fence))) ||
        FAILED(state->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle)))
    {
        spdlog::error("PyroWave: shared D3D11 fence creation failed");
        return false;
    }

    // Encode on the adapter the eyes are staged on, which is the app's.
    pyrowave_luid luid = {};
    if (!Win32DeviceAdapterLuid(state->device.Get(), luid.luid) ||
        pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &luid, &state->device_) != PYROWAVE_SUCCESS)
    {
        CloseHandle(fenceHandle);
        spdlog::error("PyroWave: no Vulkan device on the runtime adapter");
        return false;
    }

    // PyroWave takes ownership of the NT handle it imports.
    pyrowave_sync_object_create_info syncInfo = {};
    syncInfo.device = state->device_;
    syncInfo.external_handle = reinterpret_cast<pyrowave_os_handle>(fenceHandle);
    syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    if (pyrowave_sync_object_create(&syncInfo, &state->sync) != PYROWAVE_SUCCESS)
    {
        spdlog::error("PyroWave: importing the D3D11 fence failed");
        return false;
    }

    pyrowave_encoder_create_info encoderInfo = {};
    encoderInfo.device = state->device_;
    encoderInfo.width = static_cast<int>(width_);
    encoderInfo.height = static_cast<int>(height_);
    encoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&encoderInfo, &state->encoder) != PYROWAVE_SUCCESS)
    {
        spdlog::error("PyroWave: encoder {}x{} creation failed", width_, height_);
        return false;
    }

    win32_ = state.release();
    spdlog::info("PyroWave: {}x{} @ {}Hz {}Mbps ({} bytes per frame)", width_, height_, fps_, bitrateMbps_,
                 static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    delete State(win32_);
    win32_ = nullptr;
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs,
                          std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool stereo, int64_t timestampNs,
                                  OnNalUnitCallback callback, OnFrameEncodedCallback frameCallback)
{
    Win32PyroWaveState* state = State(win32_);
    if (state == nullptr)
    {
        return false;
    }

    auto encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);
    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;
    metrics.keyframe = true;

    auto finish = [&](bool emitted) {
        frameCount_ += emitted ? 1 : 0;
        metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
        inFlightFrameCount_.fetch_sub(1);
        if (!emitted)
        {
            droppedFrameCount_.fetch_add(1);
            metrics.frameDropped = true;
        }
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return emitted;
    };

    const Win32EyeImage* left = Eye(frameSource.left);
    const Win32EyeImage* right = stereo ? Eye(frameSource.right) : nullptr;
    if (left == nullptr || (stereo && right == nullptr))
    {
        return finish(false);
    }

    D3D11_TEXTURE2D_DESC eyeDesc = {};
    left->texture->GetDesc(&eyeDesc);
    const uint32_t packedWidth = stereo ? left->width * 2 : left->width;
    if (!state->EnsurePacked(eyeDesc.Format, packedWidth, left->height))
    {
        return finish(false);
    }

    // GPU: pack the eyes, then signal the fence PyroWave's acquire waits on.
    auto copyStart = Clock::now();
    state->context->CopySubresourceRegion(state->packed.Get(), 0, 0, 0, 0, left->texture, 0, nullptr);
    if (stereo)
    {
        state->context->CopySubresourceRegion(state->packed.Get(), 0, left->width, 0, 0, right->texture, 0, nullptr);
    }
    const uint64_t value = ++state->fenceValue;
    state->context->Signal(state->fence.Get(), value);
    state->context->Flush();
    metrics.gpuCopyMs = ToMilliseconds(Clock::now() - copyStart);

    pyrowave_gpu_external_reference reference = {state->image, VK_QUEUE_FAMILY_EXTERNAL};
    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = &reference;
    acquire.num_images = 1;
    acquire.sync = {pyrowave_sync_object_get_semaphore(state->sync), value};
    pyrowave_gpu_sync_operation release = {};
    release.images = &reference;
    release.num_images = 1;

    pyrowave_scaled_encode_info scaled = {};
    scaled.view = state->view;
    scaled.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.intermediate_plane_format = VK_FORMAT_R8_UNORM;
    scaled.ycbcr_chroma_midpoint = 0.5f;

    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_;

    auto submitStart = Clock::now();
    pyrowave_result result =
        pyrowave_encoder_encode_gpu_scaled_synchronous(state->encoder, &acquire, &release, &scaled, &rate);
    size_t packetCount = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        result = pyrowave_encoder_compute_num_packets(state->encoder, PacketBoundary, &packetCount);
    }
    size_t written = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        state->packets.resize(packetCount);
        state->bitstream.resize(rate.maximum_bitstream_size + PacketBoundary);
        result = pyrowave_encoder_packetize(state->encoder, state->packets.data(), PacketBoundary, &written,
                                            state->bitstream.data(), state->bitstream.size());
    }
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
    if (result != PYROWAVE_SUCCESS || written == 0)
    {
        spdlog::warn("PyroWave: encode failed ({})", static_cast<int>(result));
        return finish(false);
    }

    const pyrowave_packet& last = state->packets[written - 1];
    const size_t bytes = last.offset + last.size;
    if (metrics.frameNumber < 5 || metrics.frameNumber % 600 == 0)
    {
        spdlog::info("PyroWave: frame {} {} bytes (budget {}), encode {:.2f} ms, pack {:.2f} ms", metrics.frameNumber,
                     bytes, rate.maximum_bitstream_size, metrics.encodeSubmitMs, metrics.gpuCopyMs);
    }
    forceKeyframe_.store(false); // every PyroWave frame is intra-only
    if (callback)
    {
        callback(state->bitstream.data(), bytes, true, timestampNs);
    }
    return finish(true);
}

void VideoEncoder::ForceKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    outSlotIndex = 0;
    return false;
}

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/)
{
}

void VideoEncoder::DestroySlots()
{
}
