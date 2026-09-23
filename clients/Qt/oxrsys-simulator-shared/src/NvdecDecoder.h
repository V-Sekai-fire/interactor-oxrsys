// SPDX-License-Identifier: MPL-2.0
//
// AV1 decode on NVDEC for the Windows simulator: the runtime's NVENC AV1 stream in,
// RGB QImages out. nvcuda.dll and nvcuvid.dll are loaded from the driver at run time
// (V-Sekai-fire/nv-codec-headers' dynlink loader), so nothing is linked.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>

#include <cstdint>
#include <vector>

struct CudaFunctions;
struct CuvidFunctions;

class NvdecDecoder final
{
public:
    NvdecDecoder() = default;
    ~NvdecDecoder();

    NvdecDecoder(const NvdecDecoder&) = delete;
    NvdecDecoder& operator=(const NvdecDecoder&) = delete;

    // Load the driver libraries and create the CUDA context and AV1 parser.
    bool initialize(QString* error);
    bool isInitialized() const { return parser_ != nullptr; }

    // Feed one encoded frame (a temporal unit); decoded frames are appended to frames.
    bool decode(const QByteArray& data, int64_t presentationTimeNs, QList<QImage>& frames);

    void reset();

private:
    static int __stdcall handleSequence(void* user, void* format);
    static int __stdcall handleDecode(void* user, void* picture);
    static int __stdcall handleDisplay(void* user, void* display);

    int onSequence(void* format);
    int onDecode(void* picture);
    int onDisplay(void* display);

    CudaFunctions* cuda_ = nullptr;
    CuvidFunctions* cuvid_ = nullptr;
    void* context_ = nullptr;  // CUcontext
    void* lock_ = nullptr;     // CUvideoctxlock
    void* parser_ = nullptr;   // CUvideoparser
    void* decoder_ = nullptr;  // CUvideodecoder
    unsigned codedWidth_ = 0;
    unsigned codedHeight_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
    std::vector<uint8_t> nv12_;
    QList<QImage>* pending_ = nullptr;
    bool failed_ = false;
};
