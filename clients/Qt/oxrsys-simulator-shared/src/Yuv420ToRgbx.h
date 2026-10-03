// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>

// Runs the Lean-authored yuv420_to_rgbx kernel's C++ target (kernels/simulator) over one frame.
// Each plane is packed four bytes to a word and padded to whole words.
void yuv420ToRgbx(const uint32_t* luma, size_t lumaWords, const uint32_t* cb, const uint32_t* cr, size_t chromaWords,
                  uint32_t width, uint32_t height, uint32_t* rgbx);
