// SPDX-License-Identifier: MPL-2.0
//
// Windows video encoder: NVENC AV1 from the runtime's D3D11 device, no pixel ever on the CPU.
//
// The eyes arrive as D3D11 textures (D3D11Interop.cpp). The D3D11 video processor scales
// each into its half of one NV12 texture and converts BT.709 limited range; NVENC encodes
// that texture as AV1. NVENC is reached through the driver's own nvEncodeAPI64.dll at run
// time -- NvEncodeAPICreateInstance and NvEncodeAPIGetMaxSupportedVersion, the two exports
// contract-pixel-stream's nvenc.sigs lists -- so nothing is linked and a machine without an
// encoder fails at Initialize, not at build. Struct layouts come from
// V-Sekai-fire/nv-codec-headers. AV1 rather than HEVC: AV1 carries the AOMedia patent
// grant, HEVC carries pools (contract-pixel-stream docs/codec-measurements.md).

#include "VideoEncoder.h"
#include "Config.h"
#include "D3D11Interop.h"

#include <oxrsys/protocol/Protocol.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <cstdarg>
#include <cstdio>

// The loader logs printf-style; route it to spdlog.
static void LogNvLoader(const char* format, ...)
{
    char message[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    spdlog::error("NVENC loader: {}", message);
}
#define FFNV_LOG_FUNC(logctx, msg, ...) LogNvLoader((msg), __VA_ARGS__)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...)
#include <ffnvcodec/dynlink_loader.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

bool SameGuid(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// The driver's function table, loaded once per process.
struct NvencApi
{
    NvencFunctions* functions = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api = {};
    bool ok = false;
};

const NvencApi& GetNvencApi()
{
    static NvencApi instance = [] {
        NvencApi loaded;
        if (nvenc_load_functions(&loaded.functions, nullptr) != 0)
        {
            spdlog::error("NVENC: nvEncodeAPI64.dll not available (no NVIDIA driver?)");
            return loaded;
        }
        uint32_t maxVersion = 0;
        loaded.functions->NvEncodeAPIGetMaxSupportedVersion(&maxVersion);
        const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (maxVersion < headerVersion)
        {
            spdlog::error("NVENC: driver supports API {}.{}, headers need {}.{}; update the driver",
                          maxVersion >> 4, maxVersion & 0xf, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return loaded;
        }
        loaded.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (loaded.functions->NvEncodeAPICreateInstance(&loaded.api) != NV_ENC_SUCCESS)
        {
            spdlog::error("NVENC: NvEncodeAPICreateInstance failed");
            return loaded;
        }
        loaded.ok = true;
        return loaded;
    }();
    return instance;
}

void* OpenSession(const NvencApi& nv, void* d3d11Device)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = d3d11Device;
    params.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    if (nv.api.nvEncOpenEncodeSessionEx(&params, &encoder) != NV_ENC_SUCCESS)
    {
        return nullptr;
    }
    return encoder;
}

bool SessionHasAv1(const NvencApi& nv, void* encoder)
{
    uint32_t count = 0;
    if (nv.api.nvEncGetEncodeGUIDCount(encoder, &count) != NV_ENC_SUCCESS || count == 0)
    {
        return false;
    }
    std::vector<GUID> guids(count);
    uint32_t written = 0;
    if (nv.api.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &written) != NV_ENC_SUCCESS)
    {
        return false;
    }
    return std::any_of(guids.begin(), guids.begin() + written,
                       [](const GUID& guid) { return SameGuid(guid, NV_ENC_CODEC_AV1_GUID); });
}

// Everything NVENC and the video processor hold for one encoder.
struct NvencState
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;

    ComPtr<ID3D11VideoProcessorEnumerator> vpEnumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    uint32_t vpInputWidth = 0;
    uint32_t vpInputHeight = 0;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inputViews;

    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;

    void* encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    NV_ENC_INITIALIZE_PARAMS initParams = {};
    NV_ENC_CONFIG config = {};

    ~NvencState()
    {
        const NvencApi& nv = GetNvencApi();
        if (encoder != nullptr && nv.ok)
        {
            if (bitstream != nullptr)
            {
                nv.api.nvEncDestroyBitstreamBuffer(encoder, bitstream);
            }
            if (registered != nullptr)
            {
                nv.api.nvEncUnregisterResource(encoder, registered);
            }
            nv.api.nvEncDestroyEncoder(encoder);
        }
    }

    // (Re)create the video processor for this eye size: RGB full range in, BT.709 limited
    // NV12 out, scaled to the encoded eye size.
    bool EnsureProcessor(uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth, uint32_t outputHeight)
    {
        if (processor && inputWidth == vpInputWidth && inputHeight == vpInputHeight)
        {
            return true;
        }
        inputViews.clear();
        processor.Reset();
        vpEnumerator.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = inputWidth;
        desc.InputHeight = inputHeight;
        desc.OutputWidth = outputWidth;
        desc.OutputHeight = outputHeight;
        desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&desc, &vpEnumerator)) ||
            FAILED(videoDevice->CreateVideoProcessor(vpEnumerator.Get(), 0, &processor)))
        {
            spdlog::error("NVENC: D3D11 video processor creation failed");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input = {};
        input.RGB_Range = 0; // full
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output = {};
        output.YCbCr_Matrix = 1;  // BT.709
        output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &input);
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &output);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputView.Reset();
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(nv12.Get(), vpEnumerator.Get(), &outputDesc,
                                                               &outputView)))
        {
            spdlog::error("NVENC: video processor output view creation failed");
            processor.Reset();
            return false;
        }
        vpInputWidth = inputWidth;
        vpInputHeight = inputHeight;
        return true;
    }

    ID3D11VideoProcessorInputView* InputView(ID3D11Texture2D* texture)
    {
        auto it = inputViews.find(texture);
        if (it != inputViews.end())
        {
            return it->second.Get();
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> view;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(texture, vpEnumerator.Get(), &desc, &view)))
        {
            return nullptr;
        }
        // Eye textures are pooled (three per slice), so this map stays small.
        inputViews[texture] = view;
        return view.Get();
    }

    // Scale and convert one eye into columns [x, x + width) of the NV12 texture.
    bool Blit(const Win32EyeImage& eye, LONG x, LONG width, LONG height)
    {
        ID3D11VideoProcessorInputView* view = InputView(eye.texture);
        if (view == nullptr)
        {
            return false;
        }
        const RECT source = {0, 0, static_cast<LONG>(eye.width), static_cast<LONG>(eye.height)};
        const RECT target = {x, 0, x + width, height};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &target);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &target);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = view;
        return SUCCEEDED(videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream));
    }
};

NvencState* State(void* ptr)
{
    return static_cast<NvencState*>(ptr);
}

const Win32EyeImage* Eye(const FrameImageSource& source)
{
    return source.IsValid() ? static_cast<const Win32EyeImage*>(source.GetImage()) : nullptr;
}

} // namespace

bool NvencSupportsAv1(void* d3d11Device)
{
    const NvencApi& nv = GetNvencApi();
    if (!nv.ok || d3d11Device == nullptr)
    {
        return false;
    }
    void* encoder = OpenSession(nv, d3d11Device);
    if (encoder == nullptr)
    {
        return false; // not an NVIDIA adapter, or no encoder on it
    }
    const bool av1 = SessionHasAv1(nv, encoder);
    nv.api.nvEncDestroyEncoder(encoder);
    return av1;
}

VideoEncoder::VideoEncoder() = default;

uint8_t VideoEncoder::StreamCodec()
{
    return static_cast<uint8_t>(oxr::protocol::VideoCodec::AV1);
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

    // NV12 and the side-by-side split both want even sizes.
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

    const NvencApi& nv = GetNvencApi();
    if (!nv.ok || graphicsContext.api != GraphicsApi::Vulkan)
    {
        return false;
    }

    auto state = std::make_unique<NvencState>();
    state->device = static_cast<ID3D11Device*>(Win32InteropD3D11Device(graphicsContext.vulkan));
    if (!state->device)
    {
        spdlog::error("NVENC: no runtime D3D11 device");
        return false;
    }
    state->device->GetImmediateContext(&state->context);
    if (FAILED(state->device.As(&state->videoDevice)) || FAILED(state->context.As(&state->videoContext)))
    {
        spdlog::error("NVENC: the runtime D3D11 device has no video support");
        return false;
    }

    state->encoder = OpenSession(nv, state->device.Get());
    if (state->encoder == nullptr || !SessionHasAv1(nv, state->encoder))
    {
        spdlog::error("NVENC: this adapter has no AV1 encoder (AV1 encode needs Ada or newer)");
        return false;
    }

    const std::string encoderPreset = Config::Get().GetValues().encoderPreset;
    const GUID preset = encoderPreset == "speed"     ? NV_ENC_PRESET_P1_GUID
                        : encoderPreset == "quality" ? NV_ENC_PRESET_P4_GUID
                                                     : NV_ENC_PRESET_P2_GUID;

    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    if (nv.api.nvEncGetEncodePresetConfigEx(state->encoder, NV_ENC_CODEC_AV1_GUID, preset,
                                            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig) != NV_ENC_SUCCESS)
    {
        spdlog::error("NVENC: AV1 preset query failed");
        return false;
    }

    // Low latency: no B-frames, no automatic keyframes (the client asks for them), CBR with a
    // one-frame VBV so a frame never waits on the rate controller.
    NV_ENC_CONFIG& config = state->config;
    config = presetConfig.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = bitrateMbps_ * 1'000'000u;
    config.rcParams.maxBitRate = config.rcParams.averageBitRate;
    config.rcParams.vbvBufferSize = config.rcParams.averageBitRate / fps_;
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    NV_ENC_CONFIG_AV1& av1 = config.encodeCodecConfig.av1Config;
    av1.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    av1.repeatSeqHdr = 1;
    av1.chromaFormatIDC = 1; // 4:2:0
    av1.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    av1.colorRange = 0; // limited, as the video processor writes it

    NV_ENC_INITIALIZE_PARAMS& init = state->initParams;
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_AV1_GUID;
    init.presetGUID = preset;
    init.encodeWidth = width_;
    init.encodeHeight = height_;
    init.darWidth = width_;
    init.darHeight = height_;
    init.frameRateNum = fps_;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config;
    init.maxEncodeWidth = width_;
    init.maxEncodeHeight = height_;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    NVENCSTATUS status = nv.api.nvEncInitializeEncoder(state->encoder, &init);
    if (status != NV_ENC_SUCCESS)
    {
        spdlog::error("NVENC: nvEncInitializeEncoder failed ({})", static_cast<int>(status));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(state->device->CreateTexture2D(&desc, nullptr, &state->nv12)))
    {
        spdlog::error("NVENC: NV12 texture creation failed");
        return false;
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = width_;
    reg.height = height_;
    reg.resourceToRegister = state->nv12.Get();
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nv.api.nvEncRegisterResource(state->encoder, &reg) != NV_ENC_SUCCESS)
    {
        spdlog::error("NVENC: registering the NV12 texture failed");
        return false;
    }
    state->registered = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream = {};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (nv.api.nvEncCreateBitstreamBuffer(state->encoder, &bitstream) != NV_ENC_SUCCESS)
    {
        spdlog::error("NVENC: bitstream buffer creation failed");
        return false;
    }
    state->bitstream = bitstream.bitstreamBuffer;

    nvenc_ = state.release();
    spdlog::info("NVENC: AV1 {}x{} @ {}Hz {}Mbps CBR preset={}", width_, height_, fps_, bitrateMbps_,
                 encoderPreset);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    delete State(nvenc_);
    nvenc_ = nullptr;
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
    NvencState* state = State(nvenc_);
    if (state == nullptr)
    {
        return false;
    }
    const NvencApi& nv = GetNvencApi();

    auto encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);
    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;

    auto drop = [&]() {
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    };

    const Win32EyeImage* left = Eye(frameSource.left);
    const Win32EyeImage* right = stereo ? Eye(frameSource.right) : nullptr;
    if (left == nullptr || (stereo && right == nullptr))
    {
        return drop();
    }

    // GPU: scale and convert the eyes into the NV12 texture.
    auto copyStart = Clock::now();
    const LONG eyeWidth = static_cast<LONG>(stereo ? eyeWidth_ : width_);
    if (!state->EnsureProcessor(left->width, left->height, static_cast<uint32_t>(eyeWidth), height_) ||
        !state->Blit(*left, 0, eyeWidth, static_cast<LONG>(height_)) ||
        (stereo && !state->Blit(*right, eyeWidth, eyeWidth, static_cast<LONG>(height_))))
    {
        return drop();
    }
    metrics.gpuCopyMs = ToMilliseconds(Clock::now() - copyStart);

    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = state->registered;
    if (nv.api.nvEncMapInputResource(state->encoder, &map) != NV_ENC_SUCCESS)
    {
        return drop();
    }

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = state->bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = frameCount_;
    if (forceKeyframe_.exchange(false))
    {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    auto submitStart = Clock::now();
    const NVENCSTATUS status = nv.api.nvEncEncodePicture(state->encoder, &pic);
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);
    bool emitted = false;
    if (status == NV_ENC_SUCCESS)
    {
        NV_ENC_LOCK_BITSTREAM lock = {};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = state->bitstream;
        if (nv.api.nvEncLockBitstream(state->encoder, &lock) == NV_ENC_SUCCESS)
        {
            const bool keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
            metrics.keyframe = keyframe;
            if (callback && lock.bitstreamSizeInBytes > 0)
            {
                callback(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes, keyframe,
                         timestampNs);
                emitted = true;
            }
            nv.api.nvEncUnlockBitstream(state->encoder, state->bitstream);
        }
    }
    nv.api.nvEncUnmapInputResource(state->encoder, map.mappedResource);

    frameCount_++;
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
}

void VideoEncoder::ForceKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
    NvencState* state = State(nvenc_);
    if (state == nullptr)
    {
        return;
    }
    state->config.rcParams.averageBitRate = bitrateMbps_ * 1'000'000u;
    state->config.rcParams.maxBitRate = state->config.rcParams.averageBitRate;
    state->config.rcParams.vbvBufferSize = state->config.rcParams.averageBitRate / fps_;
    state->config.rcParams.vbvInitialDelay = state->config.rcParams.vbvBufferSize;

    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfigure.reInitEncodeParams = state->initParams;
    reconfigure.reInitEncodeParams.encodeConfig = &state->config;
    if (GetNvencApi().api.nvEncReconfigureEncoder(state->encoder, &reconfigure) != NV_ENC_SUCCESS)
    {
        spdlog::warn("NVENC: bitrate change to {}Mbps refused", bitrateMbps_);
    }
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
