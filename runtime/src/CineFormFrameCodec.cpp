// SPDX-License-Identifier: MPL-2.0

#include "CineFormFrameCodec.h"

// CFHDAllocator.h uses size_t without including a header that declares it.
#include <cstddef>

#include <CFHDEncoder.h>

CineFormFrameCodec::~CineFormFrameCodec()
{
    Close();
}

CineFormFrameCodec::Quality CineFormFrameCodec::QualityForPreset(std::string_view encoderPreset)
{
    if (encoderPreset == "speed")
    {
        return Quality::Low;
    }
    if (encoderPreset == "quality")
    {
        return Quality::High;
    }
    return Quality::Medium;
}

bool CineFormFrameCodec::Open(uint32_t width, uint32_t height, Quality quality, std::string& error)
{
    Close();

    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0)
    {
        error =
            "CineForm needs a non-empty even frame size, got " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }

    CFHD_EncodingQuality encodingQuality = CFHD_ENCODING_QUALITY_MEDIUM;
    if (quality == Quality::Low)
    {
        encodingQuality = CFHD_ENCODING_QUALITY_LOW;
    }
    else if (quality == Quality::High)
    {
        encodingQuality = CFHD_ENCODING_QUALITY_HIGH;
    }

    CFHD_EncoderRef encoder = nullptr;
    CFHD_Error result = CFHD_OpenEncoder(&encoder, nullptr);
    if (result != CFHD_ERROR_OKAY || encoder == nullptr)
    {
        error = "CFHD_OpenEncoder failed with code " + std::to_string(static_cast<int>(result));
        return false;
    }

    result = CFHD_PrepareToEncode(encoder, static_cast<int>(width), static_cast<int>(height), CFHD_PIXEL_FORMAT_BGRA,
                                  CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, encodingQuality);
    if (result != CFHD_ERROR_OKAY)
    {
        CFHD_CloseEncoder(encoder);
        error = "CFHD_PrepareToEncode failed with code " + std::to_string(static_cast<int>(result));
        return false;
    }

    encoder_ = encoder;
    width_ = width;
    height_ = height;
    return true;
}

void CineFormFrameCodec::Close()
{
    if (encoder_ != nullptr)
    {
        CFHD_CloseEncoder(encoder_);
        encoder_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

bool CineFormFrameCodec::EncodeBgra(const uint8_t* bgra, int32_t pitch, const uint8_t*& sample, size_t& sampleSize,
                                    std::string& error)
{
    sample = nullptr;
    sampleSize = 0;
    if (encoder_ == nullptr)
    {
        error = "CineForm encoder is not open";
        return false;
    }

    CFHD_Error result = CFHD_EncodeSample(encoder_, const_cast<uint8_t*>(bgra), pitch);
    if (result != CFHD_ERROR_OKAY)
    {
        error = "CFHD_EncodeSample failed with code " + std::to_string(static_cast<int>(result));
        return false;
    }

    void* data = nullptr;
    size_t size = 0;
    result = CFHD_GetSampleData(encoder_, &data, &size);
    if (result != CFHD_ERROR_OKAY || data == nullptr || size == 0)
    {
        error = "CFHD_GetSampleData failed with code " + std::to_string(static_cast<int>(result));
        return false;
    }

    sample = static_cast<const uint8_t*>(data);
    sampleSize = size;
    return true;
}
