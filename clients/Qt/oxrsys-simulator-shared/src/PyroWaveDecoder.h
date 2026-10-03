// SPDX-License-Identifier: MPL-2.0
//
// PyroWave decode for the Qt simulator, on the GPU: PyroWave decodes into three plane images on a
// Vulkan device it creates itself, and the Lean-authored yuv420_to_rgbx kernel (kernels/simulator)
// turns them into RGBX in a host-visible buffer that becomes the QImage.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>

#include <cstdint>
#include <memory>

class PyroWaveDecoder final
{
public:
    PyroWaveDecoder();
    ~PyroWaveDecoder();

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    bool initialize(QString* error);
    bool isInitialized() const;

    // Decode one stream frame; the image is appended to frames.
    bool decode(const QByteArray& data, int64_t presentationTimeNs, QList<QImage>& frames);

    void reset();

    // Width and height from the frame's sequence header, or false without one.
    static bool frameSize(const QByteArray& data, int& width, int& height);

private:
    struct Gpu;
    std::unique_ptr<Gpu> gpu_;
};
