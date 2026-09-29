// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "CineFormFrameCodec.h"
#include "VideoEncoder.h"

#include <cstddef>

#include <CFHDDecoder.h>
#include <CFHDMetadata.h>
#include <CFHDMetadataTags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

    // Bounds over the 8-bit B, G and R channels. The printed values are the measurement;
    // the largest single-pixel errors sit on the hard red block edges, where 4:2:2 chroma
    // cannot follow, so the bound is on PSNR and the mean rather than the maximum.
    constexpr double kMinPsnrDb = 38.0;
    constexpr double kMeanErrorBound = 1.5;
    constexpr uint32_t kWidth = 1024;
    constexpr uint32_t kHeight = 576;

    std::vector<uint8_t> TestFrame(uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t* p = frame.data() + (static_cast<size_t>(y) * width + x) * 4u;
                const bool block = ((x / 32u) + (y / 32u)) % 2u == 0u && x > width / 2u && y > height / 2u;
                p[0] = static_cast<uint8_t>((x + y) * 255u / (width + height));
                p[1] = static_cast<uint8_t>(y * 255u / height);
                p[2] = block ? 230 : static_cast<uint8_t>(x * 255u / width);
                p[3] = 255;
            }
        }
        return frame;
    }

    struct RoundTrip
    {
        bool decoded = false;
        std::string error;
        double meanError = 0.0;
        double psnrDb = 0.0;
        int maxError = 0;

        bool WithinBound() const
        {
            return decoded && psnrDb >= kMinPsnrDb && meanError <= kMeanErrorBound;
        }
    };

    RoundTrip DecodeAndCompare(const uint8_t* sample, size_t sampleSize, const std::vector<uint8_t>& source,
                               uint32_t width, uint32_t height)
    {
        RoundTrip result;
        CFHD_DecoderRef decoder = nullptr;
        if (CFHD_OpenDecoder(&decoder, nullptr) != CFHD_ERROR_OKAY)
        {
            result.error = "CFHD_OpenDecoder failed";
            return result;
        }

        std::vector<uint8_t> copy(sample, sample + sampleSize);
        int actualWidth = 0;
        int actualHeight = 0;
        CFHD_PixelFormat actualFormat = CFHD_PIXEL_FORMAT_UNKNOWN;
        CFHD_Error prepared =
            CFHD_PrepareToDecode(decoder, static_cast<int>(width), static_cast<int>(height), CFHD_PIXEL_FORMAT_BGRA,
                                 CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE, copy.data(), copy.size(),
                                 &actualWidth, &actualHeight, &actualFormat);
        if (prepared != CFHD_ERROR_OKAY)
        {
            result.error = "CFHD_PrepareToDecode returned " + std::to_string(static_cast<int>(prepared));
            CFHD_CloseDecoder(decoder);
            return result;
        }
        if (actualWidth != static_cast<int>(width) || actualHeight != static_cast<int>(height) ||
            actualFormat != CFHD_PIXEL_FORMAT_BGRA)
        {
            result.error = "decoder chose " + std::to_string(actualWidth) + "x" + std::to_string(actualHeight);
            CFHD_CloseDecoder(decoder);
            return result;
        }

        // The SDK's multithreaded decode intermittently returns a wrong image of a correct
        // sample (measured 12 of 160 decodes on 16 CPUs, 0 of 160 on one), so the check decodes on one.
        CFHD_MetadataRef metadata = nullptr;
        uint32_t oneCpu = 1;
        if (CFHD_OpenMetadata(&metadata) != CFHD_ERROR_OKAY ||
            CFHD_SetActiveMetadata(decoder, metadata, TAG_CPU_MAX, METADATATYPE_UINT32, &oneCpu, sizeof(oneCpu)) !=
                CFHD_ERROR_OKAY)
        {
            result.error = "could not limit the decoder to one CPU";
            if (metadata != nullptr)
            {
                CFHD_CloseMetadata(metadata);
            }
            CFHD_CloseDecoder(decoder);
            return result;
        }

        std::vector<uint8_t> output(source.size(), 0);
        CFHD_Error decoded =
            CFHD_DecodeSample(decoder, copy.data(), copy.size(), output.data(), static_cast<int32_t>(width * 4u));
        CFHD_CloseDecoder(decoder);
        CFHD_CloseMetadata(metadata);
        if (decoded != CFHD_ERROR_OKAY)
        {
            result.error = "CFHD_DecodeSample returned " + std::to_string(static_cast<int>(decoded));
            return result;
        }

        uint64_t sum = 0;
        uint64_t squares = 0;
        uint64_t count = 0;
        for (size_t i = 0; i < source.size(); i += 4)
        {
            for (size_t c = 0; c < 3; ++c)
            {
                const int difference = std::abs(static_cast<int>(output[i + c]) - static_cast<int>(source[i + c]));
                result.maxError = std::max(result.maxError, difference);
                sum += static_cast<uint64_t>(difference);
                squares += static_cast<uint64_t>(difference) * static_cast<uint64_t>(difference);
                ++count;
            }
        }
        result.decoded = true;
        result.meanError = static_cast<double>(sum) / static_cast<double>(count);
        const double mse = static_cast<double>(squares) / static_cast<double>(count);
        result.psnrDb = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        return result;
    }

    std::vector<uint8_t> EncodeTestFrame(CineFormFrameCodec::Quality quality, const std::vector<uint8_t>& frame,
                                         double& encodeMs)
    {
        CineFormFrameCodec codec;
        std::string error;
        REQUIRE(codec.Open(kWidth, kHeight, quality, error));
        const uint8_t* sample = nullptr;
        size_t sampleSize = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        REQUIRE(codec.EncodeBgra(frame.data(), static_cast<int32_t>(kWidth * 4u), sample, sampleSize, error));
        encodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return std::vector<uint8_t>(sample, sample + sampleSize);
    }

} // namespace

TEST_CASE("A synthetic frame survives a CineForm round trip within the bound", "[cineform]")
{
    const std::vector<uint8_t> frame = TestFrame(kWidth, kHeight);
    const CineFormFrameCodec::Quality qualities[] = {
        CineFormFrameCodec::Quality::Low,
        CineFormFrameCodec::Quality::Medium,
        CineFormFrameCodec::Quality::High,
    };
    const char* names[] = {"low", "medium", "high"};
    for (size_t q = 0; q < 3; ++q)
    {
        double encodeMs = 0.0;
        const std::vector<uint8_t> sample = EncodeTestFrame(qualities[q], frame, encodeMs);
        const RoundTrip trip = DecodeAndCompare(sample.data(), sample.size(), frame, kWidth, kHeight);
        std::printf("cineform round trip %s: %ux%u sample %zu bytes (%.2f bits/pixel), encode %.2f ms, "
                    "PSNR %.2f dB, mean |error| %.3f, max |error| %d (bound PSNR >= %.1f dB, mean <= %.1f)\n",
                    names[q], kWidth, kHeight, sample.size(),
                    static_cast<double>(sample.size()) * 8.0 / (kWidth * kHeight), encodeMs, trip.psnrDb,
                    trip.meanError, trip.maxError, kMinPsnrDb, kMeanErrorBound);
        INFO(trip.error);
        CHECK(trip.WithinBound());
    }
}

TEST_CASE("The Linux VideoEncoder streams CineForm keyframes of its placeholder frame", "[cineform]")
{
    REQUIRE(VideoEncoder::StreamCodec() == oxr::protocol::VideoCodec::CineForm);

    VideoEncoder encoder;
    REQUIRE(encoder.Initialize(640, 360, 90, 50, GraphicsContext{}));
    REQUIRE(encoder.IsInitialized());

    std::vector<uint8_t> received;
    bool keyframe = false;
    int64_t timestamp = 0;
    const bool encoded = encoder.Encode(FrameImageSource{}, 1234,
                                        [&](const uint8_t* data, size_t size, bool isKeyframe, int64_t timestampNs)
                                        {
                                            received.assign(data, data + size);
                                            keyframe = isKeyframe;
                                            timestamp = timestampNs;
                                        });
    REQUIRE(encoded);
    REQUIRE_FALSE(received.empty());
    CHECK(keyframe);
    CHECK(timestamp == 1234);
    CHECK(encoder.GetEncodedFrameCount() == 1);

    std::vector<uint8_t> black(static_cast<size_t>(640) * 360 * 4u, 0);
    for (size_t i = 3; i < black.size(); i += 4)
    {
        black[i] = 255;
    }
    const RoundTrip trip = DecodeAndCompare(received.data(), received.size(), black, 640, 360);
    std::printf("cineform placeholder frame: sample %zu bytes, PSNR %.2f dB, mean |error| %.3f, max |error| %d\n",
                received.size(), trip.psnrDb, trip.meanError, trip.maxError);
    INFO(trip.error);
    CHECK(trip.WithinBound());
}

TEST_CASE("A corrupted CineForm sample fails the round-trip check", "[cineform][control]")
{
    const std::vector<uint8_t> frame = TestFrame(kWidth, kHeight);
    double encodeMs = 0.0;
    const std::vector<uint8_t> sample = EncodeTestFrame(CineFormFrameCodec::Quality::Medium, frame, encodeMs);
    REQUIRE(DecodeAndCompare(sample.data(), sample.size(), frame, kWidth, kHeight).WithinBound());

    struct Corruption
    {
        const char* name;
        std::vector<uint8_t> bytes;
    };
    std::vector<Corruption> corruptions;

    std::vector<uint8_t> header = sample;
    std::fill(header.begin(), header.begin() + 64, static_cast<uint8_t>(0));
    corruptions.push_back({"first 64 bytes zeroed", header});

    std::vector<uint8_t> payload = sample;
    for (size_t i = payload.size() / 2; i < payload.size() / 2 + 256; ++i)
    {
        payload[i] = static_cast<uint8_t>(payload[i] ^ 0xA5u);
    }
    corruptions.push_back({"256 payload bytes flipped mid-sample", payload});

    std::vector<uint8_t> truncated(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(sample.size() / 2));
    corruptions.push_back({"truncated to half", truncated});

    for (const Corruption& corruption : corruptions)
    {
        const RoundTrip trip =
            DecodeAndCompare(corruption.bytes.data(), corruption.bytes.size(), frame, kWidth, kHeight);
        std::printf("cineform control (%s): decoded %s, PSNR %.2f dB, mean |error| %.3f, max |error| %d%s%s\n",
                    corruption.name, trip.decoded ? "yes" : "no", trip.psnrDb, trip.meanError, trip.maxError,
                    trip.error.empty() ? "" : ", ", trip.error.c_str());
        INFO(corruption.name);
        CHECK_FALSE(trip.WithinBound());
    }
}
