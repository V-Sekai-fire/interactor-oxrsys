// SPDX-License-Identifier: MPL-2.0
//
// The body hologram drawn into streamed frames: a footprint ring on the physical floor under the
// head, a faint ring at head height and a marker at each tracked hand, so the wearer sees where the
// runtime thinks their feet are against the floor the game draws.

#pragma once

#include <cstdint>
#include <vector>

struct BodyOverlay
{
    bool enabled = false;
    // The pose the frame was rendered for, in the tracking space whose floor is y = floorY.
    float headPosition[3] = {0.0f, 1.6f, 0.0f};
    float headOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyzw
    // The left eye's field of view the frame was rendered with, as tangents: left, right, up, down.
    // The right eye mirrors it.
    float eyeTangents[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    float ipd = 0.063f;
    float floorY = 0.0f;
    bool handActive[2] = {false, false};
    float handPosition[2][3] = {};
};

// A vertex of the hologram's triangles, in the packed texture's normalised coordinates
// (x and y in -1..1, y up), with straight alpha.
struct HologramVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

// Projects a world point into one eye's pixels (0 left, 1 right); false when it is behind the eye.
bool ProjectToEye(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                  float& px, float& py);

// The hologram as a triangle list for an image holding the eyes side by side (or the left eye
// alone when stereo is false), each eyeWidth by eyeHeight pixels.
std::vector<HologramVertex> BuildBodyHologram(const BodyOverlay& overlay, uint32_t eyeWidth, uint32_t eyeHeight,
                                              bool stereo);

// The overlay's field of view from a tracking packet's eye angles (left, right, up, down, radians).
void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4]);
