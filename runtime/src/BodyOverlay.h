// SPDX-License-Identifier: MPL-2.0
//
// The body hologram drawn into streamed frames: a footprint ring on the physical floor under the
// head, the play area's centre, a floor grid around the feet, a faint ring at head height, a marker at each
// tracked hand, and the client's simulated body when it sends one, so the wearer sees where the runtime puts
// their feet against what the game draws.

#pragma once

#include <cstdint>
#include <vector>

// The joints of the simulated body, in the order of oxr::protocol::BodyJoint.
constexpr int BodyJointCount = 18;

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
    bool bodyActive = false;
    uint8_t bodyContact = 0; // bit 0 the left foot, bit 1 the right, planted on the floor
    float body[BodyJointCount][3] = {};
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

// CASSIE's 4 cm proximity threshold: the play area's centre turns green with the feet this close.
constexpr float SnapMeters = 0.04f;

// xr-grid's procedural grid, on the floor around the feet: spacing, node disc radius, and its
// fade (full within XrGridFarFade of the feet, gone XrGridFadeZone further).
constexpr float XrGridStepMeters = 0.1f;
constexpr float XrGridPointRadius = 0.005f;
constexpr float XrGridFarFade = 0.2f;
constexpr float XrGridFadeZone = 0.15f;

// Whether the feet are within SnapMeters of the play area's centre.
bool FeetAtCentre(const BodyOverlay& overlay);

// The grid's opacity at a point, from its distance to the feet.
float XrGridOpacity(const BodyOverlay& overlay, const float point[3]);

// Projects a world point into one eye's pixels (0 left, 1 right); false when it is behind the eye.
bool ProjectToEye(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                  float& px, float& py);

// The hologram as a triangle list for an image holding the eyes side by side (or the left eye
// alone when stereo is false), each eyeWidth by eyeHeight pixels.
std::vector<HologramVertex> BuildBodyHologram(const BodyOverlay& overlay, uint32_t eyeWidth, uint32_t eyeHeight,
                                              bool stereo);

// The overlay's field of view from a tracking packet's eye angles (left, right, up, down, radians).
void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4]);
