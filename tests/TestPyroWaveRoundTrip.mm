// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#import <Metal/Metal.h>

#include <pyrowave_metal.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{

    constexpr int kWidth = 1024;
    constexpr int kHeight = 576;
    // 200 Mbit/s at 120 Hz, the budget a streamed frame gets.
    constexpr size_t kBudgetBytes = 200u * 1000000u / 8u / 120u;
    constexpr double kMinLumaPsnrDb = 38.0;

    struct Planes
    {
        std::vector<uint8_t> y, cb, cr;
    };

    Planes TestFrame()
    {
        Planes p;
        p.y.resize(size_t(kWidth) * kHeight);
        p.cb.resize(size_t(kWidth / 2) * (kHeight / 2));
        p.cr.resize(p.cb.size());
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                p.y[size_t(y) * kWidth + x] = uint8_t((x * 255 / kWidth + ((x / 64 + y / 64) % 2) * 40) & 0xff);
        for (int y = 0; y < kHeight / 2; ++y)
            for (int x = 0; x < kWidth / 2; ++x)
            {
                p.cb[size_t(y) * (kWidth / 2) + x] = uint8_t(64 + x * 128 / (kWidth / 2));
                p.cr[size_t(y) * (kWidth / 2) + x] = uint8_t(64 + y * 128 / (kHeight / 2));
            }
        return p;
    }

    struct Codec
    {
        id<MTLDevice> mtl = nil;
        pyrowave_device device = nullptr;
        pyrowave_encoder encoder = nullptr;
        pyrowave_decoder decoder = nullptr;

        Codec()
        {
            mtl = MTLCreateSystemDefaultDevice();
            pyrowave_device_create_info info = {};
            info.mtl_device = (__bridge void*)mtl;
            REQUIRE(pyrowave_device_create(&info, &device) == PYROWAVE_SUCCESS);
            pyrowave_encoder_create_info enc = {device, kWidth, kHeight, PYROWAVE_CHROMA_SUBSAMPLING_420};
            REQUIRE(pyrowave_encoder_create(&enc, &encoder) == PYROWAVE_SUCCESS);
            pyrowave_decoder_create_info dec = {device, kWidth, kHeight, PYROWAVE_CHROMA_SUBSAMPLING_420};
            REQUIRE(pyrowave_decoder_create(&dec, &decoder) == PYROWAVE_SUCCESS);
        }

        ~Codec()
        {
            pyrowave_decoder_destroy(decoder);
            pyrowave_encoder_destroy(encoder);
            pyrowave_device_destroy(device);
        }

        std::vector<uint8_t> Encode(Planes& p)
        {
            pyrowave_cpu_buffer in = {};
            in.data[0] = p.y.data();
            in.data[1] = p.cb.data();
            in.data[2] = p.cr.data();
            in.row_stride_in_bytes[0] = kWidth;
            in.row_stride_in_bytes[1] = in.row_stride_in_bytes[2] = kWidth / 2;
            in.plane_size_in_bytes[0] = p.y.size();
            in.plane_size_in_bytes[1] = in.plane_size_in_bytes[2] = p.cb.size();
            in.width = kWidth;
            in.height = kHeight;
            in.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
            pyrowave_rate_control rate = {kBudgetBytes};
            REQUIRE(pyrowave_encoder_encode_cpu_synchronous(encoder, &in, &rate) == PYROWAVE_SUCCESS);
            constexpr size_t boundary = 64 * 1024;
            size_t count = 0;
            REQUIRE(pyrowave_encoder_compute_num_packets(encoder, boundary, &count) == PYROWAVE_SUCCESS);
            std::vector<pyrowave_packet> packets(count);
            std::vector<uint8_t> bits(kBudgetBytes + boundary);
            size_t written = 0;
            REQUIRE(pyrowave_encoder_packetize(encoder, packets.data(), boundary, &written, bits.data(), bits.size()) ==
                    PYROWAVE_SUCCESS);
            REQUIRE(written > 0);
            bits.resize(packets[written - 1].offset + packets[written - 1].size);
            return bits;
        }

        // Luma PSNR of the decoded frame against `ref`, or a negative value when the frame is not ready.
        double DecodeLumaPsnr(const std::vector<uint8_t>& bits, const Planes& ref, pyrowave_result* pushResult)
        {
            pyrowave_decoder_clear(decoder);
            *pushResult = pyrowave_decoder_push_packet(decoder, bits.data(), bits.size());
            if (*pushResult != PYROWAVE_SUCCESS || !pyrowave_decoder_decode_is_ready(decoder, false))
                return -1.0;
            MTLTextureDescriptor* full = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                            width:kWidth
                                                                                           height:kHeight
                                                                                        mipmapped:NO];
            full.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            full.storageMode = MTLStorageModeShared;
            MTLTextureDescriptor* half = [full copy];
            half.width = kWidth / 2;
            half.height = kHeight / 2;
            id<MTLTexture> y = [mtl newTextureWithDescriptor:full];
            id<MTLTexture> cb = [mtl newTextureWithDescriptor:half];
            id<MTLTexture> cr = [mtl newTextureWithDescriptor:half];
            id<MTLCommandQueue> queue = [mtl newCommandQueue];
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            pyrowave_gpu_buffers out = {{(__bridge void*)y, (__bridge void*)cb, (__bridge void*)cr}};
            REQUIRE(pyrowave_decoder_decode_gpu_buffer(decoder, (__bridge void*)cmd, &out) == PYROWAVE_SUCCESS);
            [cmd commit];
            [cmd waitUntilCompleted];
            std::vector<uint8_t> got(ref.y.size());
            [y getBytes:got.data() bytesPerRow:kWidth fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight) mipmapLevel:0];
            double se = 0.0;
            for (size_t i = 0; i < got.size(); ++i)
            {
                const double d = double(got[i]) - double(ref.y[i]);
                se += d * d;
            }
            const double mse = std::max(se / double(got.size()), 1e-9);
            return 10.0 * std::log10(255.0 * 255.0 / mse);
        }
    };

} // namespace

TEST_CASE("PyroWave round-trips a frame within its byte budget", "[pyrowave]")
{
    Codec codec;
    Planes frame = TestFrame();
    const std::vector<uint8_t> bits = codec.Encode(frame);
    pyrowave_result push = PYROWAVE_ERROR_GENERIC;
    const double psnr = codec.DecodeLumaPsnr(bits, frame, &push);
    std::printf("PyroWave %dx%d: %zu bytes of %zu budget, luma PSNR %.2f dB\n", kWidth, kHeight, bits.size(),
                kBudgetBytes, psnr);
    CHECK(push == PYROWAVE_SUCCESS);
    CHECK(bits.size() <= kBudgetBytes);
    CHECK(psnr >= kMinLumaPsnrDb);
}

TEST_CASE("A corrupted or truncated PyroWave frame is reported, not decoded", "[pyrowave]")
{
    Codec codec;
    Planes frame = TestFrame();
    const std::vector<uint8_t> bits = codec.Encode(frame);

    std::vector<uint8_t> corrupted = bits;
    // The first header is the frame's sequence header; claiming another size must be refused.
    corrupted[0] ^= 0x01;
    pyrowave_result push = PYROWAVE_SUCCESS;
    CHECK(codec.DecodeLumaPsnr(corrupted, frame, &push) < 0.0);
    CHECK(push == PYROWAVE_ERROR_CORRUPT_BITSTREAM);

    std::vector<uint8_t> truncated(bits.begin(), bits.begin() + bits.size() / 2);
    CHECK(codec.DecodeLumaPsnr(truncated, frame, &push) < 0.0);
}
