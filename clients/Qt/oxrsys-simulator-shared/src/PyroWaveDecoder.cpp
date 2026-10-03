// SPDX-License-Identifier: MPL-2.0

#include "PyroWaveDecoder.h"
#include "Yuv420ToRgbx.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <cstring>


PyroWaveDecoder::~PyroWaveDecoder()
{
    reset();
    if (device_ != nullptr)
    {
        pyrowave_device_destroy(static_cast<pyrowave_device>(device_));
    }
}

bool PyroWaveDecoder::initialize(QString* error)
{
    if (device_ != nullptr)
    {
        return true;
    }
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS)
    {
        if (error != nullptr)
        {
            *error = "PyroWave: no Vulkan device for decoding";
        }
        return false;
    }
    device_ = device;
    return true;
}

void PyroWaveDecoder::reset()
{
    if (decoder_ != nullptr)
    {
        pyrowave_decoder_destroy(static_cast<pyrowave_decoder>(decoder_));
        decoder_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

bool PyroWaveDecoder::frameSize(const QByteArray& data, int& width, int& height)
{
    if (data.size() < 8)
    {
        return false;
    }
    uint32_t word = 0;
    std::memcpy(&word, data.constData(), sizeof(word));
    if ((word >> 31) != 1)
    {
        return false;
    }
    width = static_cast<int>(word & 0x3FFF) + 1;
    height = static_cast<int>((word >> 14) & 0x3FFF) + 1;
    return true;
}

bool PyroWaveDecoder::ensureDecoder(int width, int height)
{
    if (decoder_ != nullptr && width == width_ && height == height_)
    {
        return true;
    }
    reset();
    pyrowave_decoder_create_info info = {};
    info.device = static_cast<pyrowave_device>(device_);
    info.width = width;
    info.height = height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_decoder decoder = nullptr;
    if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
    {
        return false;
    }
    decoder_ = decoder;
    width_ = width;
    height_ = height;
    // The kernel reads whole words, so each plane is padded to a multiple of four bytes.
    const size_t chroma = static_cast<size_t>(width / 2) * (height / 2);
    planes_[0].resize(static_cast<size_t>(width) * height);
    planes_[1].resize((chroma + 3) & ~size_t(3));
    planes_[2].resize(planes_[1].size());
    return true;
}

bool PyroWaveDecoder::decode(const QByteArray& data, int64_t /*presentationTimeNs*/, QList<QImage>& frames)
{
    int width = 0;
    int height = 0;
    if (device_ == nullptr || !frameSize(data, width, height) || (width % 2) != 0 || (height % 2) != 0 ||
        !ensureDecoder(width, height))
    {
        return false;
    }

    auto* decoder = static_cast<pyrowave_decoder>(decoder_);
    pyrowave_decoder_clear(decoder);
    if (pyrowave_decoder_push_packet(decoder, data.constData(), static_cast<size_t>(data.size())) !=
            PYROWAVE_SUCCESS ||
        !pyrowave_decoder_decode_is_ready(decoder, false))
    {
        return false;
    }

    pyrowave_cpu_buffer buffer = {};
    buffer.width = width;
    buffer.height = height;
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int i = 0; i < 3; ++i)
    {
        const int planeWidth = i == 0 ? width : width / 2;
        buffer.data[i] = planes_[i].data();
        buffer.row_stride_in_bytes[i] = static_cast<size_t>(planeWidth);
        buffer.plane_size_in_bytes[i] = static_cast<size_t>(planeWidth) * (i == 0 ? height : height / 2);
    }
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS)
    {
        return false;
    }

    QImage image(width, height, QImage::Format_RGBX8888);
    yuv420ToRgbx(reinterpret_cast<const uint32_t*>(planes_[0].data()), planes_[0].size() / 4,
                 reinterpret_cast<const uint32_t*>(planes_[1].data()),
                 reinterpret_cast<const uint32_t*>(planes_[2].data()), planes_[1].size() / 4,
                 static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                 reinterpret_cast<uint32_t*>(image.bits()));
    frames.append(image);
    return true;
}
