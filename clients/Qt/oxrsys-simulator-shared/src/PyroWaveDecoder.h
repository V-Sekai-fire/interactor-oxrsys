// SPDX-License-Identifier: MPL-2.0
//
// PyroWave decode for the Qt simulator, on the GPU: PyroWave decodes into three plane images, the
// Lean-authored yuv420_to_rgbx kernel (kernels/simulator) packs them, and the left eye is blitted
// into a swapchain on the view window, so no frame comes back to the CPU.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QWindow;

class PyroWaveDecoder final
{
public:
    PyroWaveDecoder();
    ~PyroWaveDecoder();

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    bool initialize(QString* error);
    bool isInitialized() const;

    // The window frames are presented into; it takes no input.
    QWindow* createView();

    // Decode one stream frame and present it.
    bool decode(const QByteArray& data, int64_t presentationTimeNs);

    QSize decodedSize() const;

    struct OverlayBadge
    {
        QPoint position; // view device pixels
        QImage image;    // opaque, drawn over the video as is
    };

    // Badges and solid white rectangles drawn over each presented frame.
    void setOverlay(std::vector<OverlayBadge> badges, std::vector<QRect> lines);

    // The last decoded frame, read back from the GPU.
    QImage snapshot();

    void reset();

    // Width and height from the frame's sequence header, or false without one.
    static bool frameSize(const QByteArray& data, int& width, int& height);

private:
    struct Gpu;
    std::unique_ptr<Gpu> gpu_;
};
