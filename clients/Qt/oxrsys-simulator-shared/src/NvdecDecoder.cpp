// SPDX-License-Identifier: MPL-2.0

#include "NvdecDecoder.h"

#include <QtGlobal>

#include <cstdarg>
#include <cstdio>

namespace
{
void logNvLoader(const char* format, ...)
{
    char message[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    qWarning("NVDEC loader: %s", message);
}
} // namespace

#define FFNV_LOG_FUNC(logctx, msg, ...) logNvLoader((msg), __VA_ARGS__)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...)
#include <ffnvcodec/dynlink_loader.h>

#include <algorithm>

namespace
{

uint8_t clampByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// NV12, BT.709 limited range (what the runtime's video processor writes) -> RGB888.
QImage nv12ToRgb(const uint8_t* luma, const uint8_t* chroma, int pitch, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = luma + static_cast<size_t>(y) * pitch;
        const uint8_t* uv = chroma + static_cast<size_t>(y / 2) * pitch;
        uint8_t* out = image.scanLine(y);
        for (int x = 0; x < width; ++x)
        {
            const int c = (static_cast<int>(row[x]) - 16) * 298;
            const int d = static_cast<int>(uv[x & ~1]) - 128;
            const int e = static_cast<int>(uv[(x & ~1) + 1]) - 128;
            out[3 * x + 0] = clampByte((c + 459 * e + 128) >> 8);
            out[3 * x + 1] = clampByte((c - 55 * d - 136 * e + 128) >> 8);
            out[3 * x + 2] = clampByte((c + 541 * d + 128) >> 8);
        }
    }
    return image;
}

} // namespace

NvdecDecoder::~NvdecDecoder()
{
    reset();
}

bool NvdecDecoder::initialize(QString* error)
{
    if (parser_ != nullptr)
    {
        return true;
    }
    auto fail = [&](const QString& message) {
        if (error != nullptr)
        {
            *error = message;
        }
        reset();
        return false;
    };

    if (cuda_load_functions(&cuda_, nullptr) != 0 || cuvid_load_functions(&cuvid_, nullptr) != 0)
    {
        return fail("NVDEC unavailable: nvcuda.dll / nvcuvid.dll not found (no NVIDIA driver?)");
    }
    if (cuda_->cuInit(0) != CUDA_SUCCESS)
    {
        return fail("NVDEC unavailable: cuInit failed");
    }

    // The first device whose NVDEC decodes 8-bit 4:2:0 AV1.
    int deviceCount = 0;
    cuda_->cuDeviceGetCount(&deviceCount);
    CUdevice chosen = -1;
    for (int i = 0; i < deviceCount && chosen < 0; ++i)
    {
        CUdevice device = 0;
        CUcontext probe = nullptr;
        if (cuda_->cuDeviceGet(&device, i) != CUDA_SUCCESS ||
            cuda_->cuCtxCreate(&probe, 0, device) != CUDA_SUCCESS)
        {
            continue;
        }
        CUVIDDECODECAPS caps = {};
        caps.eCodecType = cudaVideoCodec_AV1;
        caps.eChromaFormat = cudaVideoChromaFormat_420;
        caps.nBitDepthMinus8 = 0;
        if (cuvid_->cuvidGetDecoderCaps != nullptr && cuvid_->cuvidGetDecoderCaps(&caps) == CUDA_SUCCESS &&
            caps.bIsSupported)
        {
            chosen = device;
            context_ = probe;
            CUcontext popped = nullptr;
            cuda_->cuCtxPopCurrent(&popped);
        }
        else
        {
            cuda_->cuCtxDestroy(probe);
        }
    }
    if (chosen < 0)
    {
        return fail("NVDEC unavailable: no GPU decodes AV1");
    }

    CUvideoctxlock lock = nullptr;
    if (cuvid_->cuvidCtxLockCreate(&lock, static_cast<CUcontext>(context_)) != CUDA_SUCCESS)
    {
        return fail("NVDEC: context lock creation failed");
    }
    lock_ = lock;

    CUVIDPARSERPARAMS params = {};
    params.CodecType = cudaVideoCodec_AV1;
    params.ulMaxNumDecodeSurfaces = 1; // raised by the sequence callback
    params.ulMaxDisplayDelay = 0;      // low latency: display as soon as decoded
    params.pUserData = this;
    params.pfnSequenceCallback = reinterpret_cast<PFNVIDSEQUENCECALLBACK>(&NvdecDecoder::handleSequence);
    params.pfnDecodePicture = reinterpret_cast<PFNVIDDECODECALLBACK>(&NvdecDecoder::handleDecode);
    params.pfnDisplayPicture = reinterpret_cast<PFNVIDDISPLAYCALLBACK>(&NvdecDecoder::handleDisplay);
    CUvideoparser parser = nullptr;
    if (cuvid_->cuvidCreateVideoParser(&parser, &params) != CUDA_SUCCESS)
    {
        return fail("NVDEC: AV1 parser creation failed");
    }
    parser_ = parser;
    return true;
}

void NvdecDecoder::reset()
{
    if (cuvid_ != nullptr)
    {
        if (parser_ != nullptr)
        {
            cuvid_->cuvidDestroyVideoParser(static_cast<CUvideoparser>(parser_));
        }
        if (decoder_ != nullptr)
        {
            cuvid_->cuvidDestroyDecoder(static_cast<CUvideodecoder>(decoder_));
        }
        if (lock_ != nullptr)
        {
            cuvid_->cuvidCtxLockDestroy(static_cast<CUvideoctxlock>(lock_));
        }
    }
    if (cuda_ != nullptr && context_ != nullptr)
    {
        cuda_->cuCtxDestroy(static_cast<CUcontext>(context_));
    }
    parser_ = nullptr;
    decoder_ = nullptr;
    lock_ = nullptr;
    context_ = nullptr;
    codedWidth_ = codedHeight_ = width_ = height_ = 0;
    cuvid_free_functions(&cuvid_);
    cuda_free_functions(&cuda_);
}

bool NvdecDecoder::decode(const QByteArray& data, int64_t presentationTimeNs, QList<QImage>& frames)
{
    if (parser_ == nullptr || data.isEmpty())
    {
        return false;
    }
    CUVIDSOURCEDATAPACKET packet = {};
    packet.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    packet.payload_size = static_cast<tcu_ulong>(data.size());
    packet.payload = reinterpret_cast<const unsigned char*>(data.constData());
    packet.timestamp = presentationTimeNs;

    const qsizetype before = frames.size();
    pending_ = &frames;
    failed_ = false;
    cuda_->cuCtxPushCurrent(static_cast<CUcontext>(context_));
    const CUresult result = cuvid_->cuvidParseVideoData(static_cast<CUvideoparser>(parser_), &packet);
    CUcontext popped = nullptr;
    cuda_->cuCtxPopCurrent(&popped);
    pending_ = nullptr;
    return result == CUDA_SUCCESS && !failed_ && frames.size() > before;
}

int __stdcall NvdecDecoder::handleSequence(void* user, void* format)
{
    return static_cast<NvdecDecoder*>(user)->onSequence(format);
}

int __stdcall NvdecDecoder::handleDecode(void* user, void* picture)
{
    return static_cast<NvdecDecoder*>(user)->onDecode(picture);
}

int __stdcall NvdecDecoder::handleDisplay(void* user, void* display)
{
    return static_cast<NvdecDecoder*>(user)->onDisplay(display);
}

int NvdecDecoder::onSequence(void* opaqueFormat)
{
    const auto* format = static_cast<const CUVIDEOFORMAT*>(opaqueFormat);
    const unsigned surfaces = std::max<unsigned>(format->min_num_decode_surfaces, 1u) + 2u;
    if (decoder_ != nullptr && format->coded_width == codedWidth_ && format->coded_height == codedHeight_)
    {
        return static_cast<int>(surfaces);
    }
    if (decoder_ != nullptr)
    {
        cuvid_->cuvidDestroyDecoder(static_cast<CUvideodecoder>(decoder_));
        decoder_ = nullptr;
    }

    codedWidth_ = format->coded_width;
    codedHeight_ = format->coded_height;
    width_ = static_cast<unsigned>(format->display_area.right - format->display_area.left);
    height_ = static_cast<unsigned>(format->display_area.bottom - format->display_area.top);

    CUVIDDECODECREATEINFO info = {};
    info.ulWidth = codedWidth_;
    info.ulHeight = codedHeight_;
    info.ulNumDecodeSurfaces = surfaces;
    info.CodecType = format->codec;
    info.ChromaFormat = format->chroma_format;
    info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    info.bitDepthMinus8 = format->bit_depth_luma_minus8;
    info.ulMaxWidth = codedWidth_;
    info.ulMaxHeight = codedHeight_;
    info.display_area.left = static_cast<short>(format->display_area.left);
    info.display_area.top = static_cast<short>(format->display_area.top);
    info.display_area.right = static_cast<short>(format->display_area.right);
    info.display_area.bottom = static_cast<short>(format->display_area.bottom);
    info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = width_;
    info.ulTargetHeight = height_;
    info.ulNumOutputSurfaces = 2;
    info.vidLock = static_cast<CUvideoctxlock>(lock_);

    CUvideodecoder decoder = nullptr;
    if (cuvid_->cuvidCreateDecoder(&decoder, &info) != CUDA_SUCCESS)
    {
        qWarning("NVDEC: cuvidCreateDecoder %ux%u failed", codedWidth_, codedHeight_);
        failed_ = true;
        return 0;
    }
    decoder_ = decoder;
    return static_cast<int>(surfaces);
}

int NvdecDecoder::onDecode(void* picture)
{
    if (decoder_ == nullptr ||
        cuvid_->cuvidDecodePicture(static_cast<CUvideodecoder>(decoder_), static_cast<CUVIDPICPARAMS*>(picture)) !=
            CUDA_SUCCESS)
    {
        failed_ = true;
        return 0;
    }
    return 1;
}

int NvdecDecoder::onDisplay(void* opaqueDisplay)
{
    const auto* display = static_cast<const CUVIDPARSERDISPINFO*>(opaqueDisplay);
    if (decoder_ == nullptr || display == nullptr)
    {
        return 0;
    }

    CUVIDPROCPARAMS proc = {};
    proc.progressive_frame = display->progressive_frame;
    proc.top_field_first = display->top_field_first;
    CUdeviceptr frame = 0;
    unsigned int pitch = 0;
    if (cuvid_->cuvidMapVideoFrame(static_cast<CUvideodecoder>(decoder_), display->picture_index, &frame, &pitch,
                                   &proc) != CUDA_SUCCESS)
    {
        failed_ = true;
        return 0;
    }

    // Luma rows, then the interleaved chroma plane.
    const size_t lumaRows = height_;
    const size_t chromaRows = (height_ + 1) / 2;
    nv12_.resize(static_cast<size_t>(pitch) * (lumaRows + chromaRows));
    CUDA_MEMCPY2D copy = {};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = frame;
    copy.srcPitch = pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = nv12_.data();
    copy.dstPitch = pitch;
    copy.WidthInBytes = width_;
    copy.Height = lumaRows;
    bool ok = cuda_->cuMemcpy2D(&copy) == CUDA_SUCCESS;
    // The output surface is ulTargetHeight rows of luma, then the chroma plane.
    copy.srcDevice = frame + static_cast<CUdeviceptr>(pitch) * height_;
    copy.dstHost = nv12_.data() + static_cast<size_t>(pitch) * lumaRows;
    copy.Height = chromaRows;
    ok = ok && cuda_->cuMemcpy2D(&copy) == CUDA_SUCCESS;
    cuvid_->cuvidUnmapVideoFrame(static_cast<CUvideodecoder>(decoder_), frame);
    if (!ok)
    {
        failed_ = true;
        return 0;
    }

    if (pending_ != nullptr)
    {
        pending_->append(nv12ToRgb(nv12_.data(), nv12_.data() + static_cast<size_t>(pitch) * lumaRows,
                                   static_cast<int>(pitch), static_cast<int>(width_), static_cast<int>(height_)));
    }
    return 1;
}
