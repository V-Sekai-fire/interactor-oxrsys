// SPDX-License-Identifier: MPL-2.0

#include "CineFormFrameCodec.h"
#include "Config.h"
#include "VideoEncoder.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace
{

    using Clock = std::chrono::steady_clock;

    double ToMilliseconds(Clock::duration duration)
    {
        return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
    }

    CineFormFrameCodec* Codec(void* ptr)
    {
        return static_cast<CineFormFrameCodec*>(ptr);
    }

    // Opaque black in BGRA.
    void FillBlackBgraFrame(std::vector<uint8_t>& frame)
    {
        for (size_t i = 0; i + 3 < frame.size(); i += 4)
        {
            frame[i + 0] = 0;
            frame[i + 1] = 0;
            frame[i + 2] = 0;
            frame[i + 3] = 255;
        }
    }

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

oxr::protocol::VideoCodec VideoEncoder::StreamCodec()
{
    return oxr::protocol::VideoCodec::CineForm;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateMbps,
                              const GraphicsContext& graphicsContext)
{
    Shutdown();

    width_ = width;
    height_ = height;
    eyeWidth_ = width / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    const std::string encoderPreset = Config::Get().GetValues().encoderPreset;
    const CineFormFrameCodec::Quality quality = CineFormFrameCodec::QualityForPreset(encoderPreset);

    CineFormFrameCodec* codec = new CineFormFrameCodec();
    std::string error;
    if (!codec->Open(width_, height_, quality, error))
    {
        spdlog::error("CineFormVideoEncoder: {}", error);
        delete codec;
        return false;
    }

    cineform_.codec = codec;
    cineform_.frame.assign(static_cast<size_t>(width_) * height_ * 4u, 0);
    FillBlackBgraFrame(cineform_.frame);

    // CineForm is intra-only with no rate control: every sample is a keyframe, and
    // the bitrate follows from the quality preset rather than bitrateMbps.
    spdlog::info("CineFormVideoEncoder: initialized {}x{} @ {}Hz preset={} (bitrate {}Mbps is not enforced)", width_,
                 height_, fps_, encoderPreset, bitrateMbps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    delete Codec(cineform_.codec);
    cineform_.codec = nullptr;
    cineform_.frame.clear();
    cineform_.frame.shrink_to_fit();
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource /*frameSource*/, bool /*stereo*/, int64_t timestampNs,
                                  OnNalUnitCallback callback, OnFrameEncodedCallback frameCallback)
{
    CineFormFrameCodec* codec = Codec(cineform_.codec);
    if (codec == nullptr || cineform_.frame.empty())
    {
        return false;
    }

    Clock::time_point encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);

    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;
    metrics.keyframe = true;
    forceKeyframe_.store(false);

    // TODO: replace the placeholder frame with a Vulkan readback of the submitted
    // images. Until then Linux streams an opaque black frame, as it did before.
    const uint8_t* sample = nullptr;
    size_t sampleSize = 0;
    std::string error;
    Clock::time_point submitStart = Clock::now();
    const bool encoded =
        codec->EncodeBgra(cineform_.frame.data(), static_cast<int32_t>(width_ * 4u), sample, sampleSize, error);
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);

    if (!encoded)
    {
        spdlog::warn("CineFormVideoEncoder: {}", error);
        droppedFrameCount_.fetch_add(1);
        inFlightFrameCount_.fetch_sub(1);
        metrics.frameDropped = true;
        if (frameCallback)
        {
            frameCallback(metrics);
        }
        return false;
    }

    if (callback)
    {
        callback(sample, sampleSize, true, timestampNs);
    }

    frameCount_++;
    metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
    inFlightFrameCount_.fetch_sub(1);

    if (frameCallback)
    {
        frameCallback(metrics);
    }
    return true;
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

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/) {}

void VideoEncoder::DestroySlots() {}
