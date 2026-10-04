// SPDX-License-Identifier: MPL-2.0
//
// Linux streaming encoder: PyroWave's C API on a Vulkan device of its own. The submitted
// swapchain images are not read back yet, so Linux streams an opaque black frame.

#include "Config.h"
#include "VideoEncoder.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr size_t PacketBoundary = 64 * 1024;

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
    return oxr::protocol::VideoCodec::PyroWave;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateMbps,
                              const GraphicsContext& graphicsContext)
{
    Shutdown();

    width_ = width & ~3u;
    height_ = height & ~1u;
    eyeWidth_ = width_ / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS)
    {
        spdlog::error("PyroWave: no Vulkan device");
        return false;
    }
    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = static_cast<int>(width_);
    info.height = static_cast<int>(height_);
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS)
    {
        pyrowave_device_destroy(device);
        spdlog::error("PyroWave: encoder {}x{} creation failed", width_, height_);
        return false;
    }
    pyrowave_.device = device;
    pyrowave_.encoder = encoder;
    spdlog::info("PyroWave: {}x{} @ {}Hz {}Mbps", width_, height_, fps_, bitrateMbps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    if (pyrowave_.encoder != nullptr)
    {
        pyrowave_encoder_destroy(static_cast<pyrowave_encoder>(pyrowave_.encoder));
        pyrowave_.encoder = nullptr;
    }
    if (pyrowave_.device != nullptr)
    {
        pyrowave_device_destroy(static_cast<pyrowave_device>(pyrowave_.device));
        pyrowave_.device = nullptr;
    }
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
    auto* encoder = static_cast<pyrowave_encoder>(pyrowave_.encoder);
    if (encoder == nullptr)
    {
        return false;
    }
    Clock::time_point encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);
    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;
    metrics.keyframe = true;

    // Opaque black in full-range YUV 4:2:0 until the submitted images are read back.
    const size_t luma = static_cast<size_t>(width_) * height_;
    std::vector<uint8_t> planes[3] = {std::vector<uint8_t>(luma, 0), std::vector<uint8_t>(luma / 4, 128),
                                      std::vector<uint8_t>(luma / 4, 128)};
    pyrowave_cpu_buffer buffer = {};
    buffer.width = static_cast<int>(width_);
    buffer.height = static_cast<int>(height_);
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int i = 0; i < 3; ++i)
    {
        buffer.data[i] = planes[i].data();
        buffer.row_stride_in_bytes[i] = i == 0 ? width_ : width_ / 2;
        buffer.plane_size_in_bytes[i] = planes[i].size();
    }
    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_;

    Clock::time_point submitStart = Clock::now();
    pyrowave_result result = pyrowave_encoder_encode_cpu_synchronous(encoder, &buffer, &rate);
    size_t packetCount = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        result = pyrowave_encoder_compute_num_packets(encoder, PacketBoundary, &packetCount);
    }
    std::vector<pyrowave_packet> packets(packetCount);
    pyrowave_.bitstream.resize(rate.maximum_bitstream_size + PacketBoundary);
    size_t written = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        result = pyrowave_encoder_packetize(encoder, packets.data(), PacketBoundary, &written,
                                            pyrowave_.bitstream.data(), pyrowave_.bitstream.size());
    }
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);

    const bool emitted = result == PYROWAVE_SUCCESS && written > 0;
    if (emitted && callback)
    {
        const pyrowave_packet& last = packets[written - 1];
        callback(pyrowave_.bitstream.data(), last.offset + last.size, true, timestampNs);
    }
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
