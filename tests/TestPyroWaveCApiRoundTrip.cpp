// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

constexpr int Width = 256;
constexpr int Height = 128;

struct Planes
{
    std::vector<uint8_t> p[3];

    Planes()
    {
        p[0].resize(Width * Height);
        p[1].resize(Width * Height / 4);
        p[2].resize(Width * Height / 4);
    }

    pyrowave_cpu_buffer Buffer()
    {
        pyrowave_cpu_buffer b = {};
        b.width = Width;
        b.height = Height;
        b.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        for (int i = 0; i < 3; ++i)
        {
            b.data[i] = p[i].data();
            b.row_stride_in_bytes[i] = i == 0 ? Width : Width / 2;
            b.plane_size_in_bytes[i] = p[i].size();
        }
        return b;
    }
};

double LumaPsnr(const Planes& a, const Planes& b)
{
    double squared = 0.0;
    for (size_t i = 0; i < a.p[0].size(); ++i)
    {
        const double d = double(a.p[0][i]) - double(b.p[0][i]);
        squared += d * d;
    }
    const double mse = squared / double(a.p[0].size());
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Encode a smooth gradient at 1 bit per pixel, optionally damage every eighth payload byte, decode.
double RoundTrip(bool corrupt)
{
    pyrowave_device device = nullptr;
    REQUIRE(pyrowave_create_default_device(&device) == PYROWAVE_SUCCESS);
    pyrowave_encoder_create_info encoderInfo = {device, Width, Height, PYROWAVE_CHROMA_SUBSAMPLING_420};
    pyrowave_encoder encoder = nullptr;
    REQUIRE(pyrowave_encoder_create(&encoderInfo, &encoder) == PYROWAVE_SUCCESS);

    Planes source;
    for (int y = 0; y < Height; ++y)
        for (int x = 0; x < Width; ++x)
            source.p[0][y * Width + x] = uint8_t((x + y) * 255 / (Width + Height));
    std::fill(source.p[1].begin(), source.p[1].end(), 128);
    std::fill(source.p[2].begin(), source.p[2].end(), 128);

    pyrowave_cpu_buffer in = source.Buffer();
    pyrowave_rate_control rate = {Width * Height / 8};
    REQUIRE(pyrowave_encoder_encode_cpu_synchronous(encoder, &in, &rate) == PYROWAVE_SUCCESS);
    size_t count = 0;
    REQUIRE(pyrowave_encoder_compute_num_packets(encoder, 64 * 1024, &count) == PYROWAVE_SUCCESS);
    std::vector<pyrowave_packet> packets(count);
    std::vector<uint8_t> bitstream(rate.maximum_bitstream_size + 64 * 1024);
    size_t written = 0;
    REQUIRE(pyrowave_encoder_packetize(encoder, packets.data(), 64 * 1024, &written, bitstream.data(),
                                       bitstream.size()) == PYROWAVE_SUCCESS);
    const size_t bytes = packets[written - 1].offset + packets[written - 1].size;
    if (corrupt)
        for (size_t i = 64; i < bytes; i += 8)
            bitstream[i] ^= 0x5A;

    pyrowave_decoder_create_info decoderInfo = {device, Width, Height, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
    pyrowave_decoder decoder = nullptr;
    REQUIRE(pyrowave_decoder_create(&decoderInfo, &decoder) == PYROWAVE_SUCCESS);
    Planes decoded;
    pyrowave_cpu_buffer out = decoded.Buffer();
    double psnr = 0.0;
    if (pyrowave_decoder_push_packet(decoder, bitstream.data(), bytes) == PYROWAVE_SUCCESS &&
        pyrowave_decoder_decode_is_ready(decoder, true) &&
        pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out) == PYROWAVE_SUCCESS)
    {
        psnr = LumaPsnr(source, decoded);
    }

    pyrowave_decoder_destroy(decoder);
    pyrowave_encoder_destroy(encoder);
    pyrowave_device_destroy(device);
    return psnr;
}

} // namespace

TEST_CASE("PyroWave round trip holds 40 dB", "[pyrowave]")
{
    CHECK(RoundTrip(false) >= 40.0);
}

TEST_CASE("A damaged bitstream falls below 40 dB", "[pyrowave]")
{
    CHECK(RoundTrip(true) < 40.0);
}
