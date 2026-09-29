// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * One CineForm encoder instance: 8-bit BGRA frames in, one intra-only sample out.
 *
 * The Linux VideoEncoder backend and the round-trip tests share this path, so the
 * tests measure the bytes the runtime streams.
 */
class CineFormFrameCodec
{
  public:
    enum class Quality
    {
        Low,
        Medium,
        High,
    };

    CineFormFrameCodec() = default;
    ~CineFormFrameCodec();

    CineFormFrameCodec(const CineFormFrameCodec&) = delete;
    CineFormFrameCodec& operator=(const CineFormFrameCodec&) = delete;

    static Quality QualityForPreset(std::string_view encoderPreset);

    bool Open(uint32_t width, uint32_t height, Quality quality, std::string& error);
    void Close();
    bool IsOpen() const
    {
        return encoder_ != nullptr;
    }

    // Rows are bottom-up, as the SDK's BGRA format expects, `pitch` bytes apart. On success
    // `sample` points into the encoder's buffer and stays valid until the next call or Close.
    bool EncodeBgra(const uint8_t* bgra, int32_t pitch, const uint8_t*& sample, size_t& sampleSize, std::string& error);

    uint32_t Width() const
    {
        return width_;
    }
    uint32_t Height() const
    {
        return height_;
    }

  private:
    void* encoder_ = nullptr; // CFHD_EncoderRef
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
