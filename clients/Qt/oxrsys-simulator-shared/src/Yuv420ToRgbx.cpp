// SPDX-License-Identifier: MPL-2.0

#include "Yuv420ToRgbx.h"

#include "yuv420_to_rgbx_emit.cpp"

void yuv420ToRgbx(const uint32_t* luma, size_t lumaWords, const uint32_t* cb, const uint32_t* cr, size_t chromaWords,
                  uint32_t width, uint32_t height, uint32_t* rgbx)
{
    Yuv420Params_0 params = {width, height};
    GlobalParams_0 globals = {};
    globals.params_0 = &params;
    globals.luma_0 = {const_cast<uint32_t*>(luma), lumaWords};
    globals.cb_0 = {const_cast<uint32_t*>(cb), chromaWords};
    globals.cr_0 = {const_cast<uint32_t*>(cr), chromaWords};
    globals.rgbx_0 = {rgbx, static_cast<size_t>(width) * height};

    ComputeVaryingInput input = {};
    input.endGroupID = {(width * height + 63) / 64, 1, 1};
    main_0(&input, nullptr, &globals);
}
