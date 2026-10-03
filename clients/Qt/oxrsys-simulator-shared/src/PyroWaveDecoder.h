// SPDX-License-Identifier: MPL-2.0
//
// PyroWave decode for the Qt simulator: one stream frame in, one RGB QImage out, decoded on a
// Vulkan device PyroWave creates itself and read back as YUV 4:2:0 planes.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>

#include <cstdint>
#include <vector>

class PyroWaveDecoder final
{
public:
    PyroWaveDecoder() = default;
    ~PyroWaveDecoder();

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    bool initialize(QString* error);
    bool isInitialized() const { return device_ != nullptr; }

    // Decode one stream frame; the image is appended to frames.
    bool decode(const QByteArray& data, int64_t presentationTimeNs, QList<QImage>& frames);

    void reset();

    // Width and height from the frame's sequence header, or false without one.
    static bool frameSize(const QByteArray& data, int& width, int& height);

private:
    bool ensureDecoder(int width, int height);

    void* device_ = nullptr;  // pyrowave_device
    void* decoder_ = nullptr; // pyrowave_decoder
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> planes_[3];
};
