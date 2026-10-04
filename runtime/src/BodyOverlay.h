// SPDX-License-Identifier: MPL-2.0
//
// The body hologram drawn into streamed frames: a footprint ring on the physical floor under the
// head, a floor grid and a 3D lattice around the play area's centre, a faint ring at head height and a marker at each
// tracked hand, so the wearer sees where the runtime puts their feet against what the game draws.

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

// The floor grid around the play area's centre: its spacing, its reach in nodes each way, and
// CASSIE's 4 cm proximity threshold for lighting the centre.
constexpr float GridStepMeters = 0.05f;
constexpr int GridHalfNodes = 8;
constexpr float SnapMeters = 0.04f;

// The 3D lattice above the floor grid: 10 cm across, a layer every 50 cm up to 2 m.
constexpr float LatticeStepMeters = 0.1f;
constexpr int LatticeHalfNodes = 4;
constexpr float LatticeLayerMeters = 0.5f;
constexpr int LatticeLayers = 4;

// The grid node nearest the feet, in steps from the centre, and whether the feet are within
// SnapMeters of the centre.
void NearestGridNode(const BodyOverlay& overlay, int& x, int& z);
bool FeetAtCentre(const BodyOverlay& overlay);

// The lattice node nearest a point, as (x steps, layer, z steps), and whether the point is within
// SnapMeters of it.
bool NearestLatticeNode(const BodyOverlay& overlay, const float point[3], int node[3]);

// Projects a world point into one eye's pixels (0 left, 1 right); false when it is behind the eye.
bool ProjectToEye(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                  float& px, float& py);

// The hologram as a triangle list for an image holding the eyes side by side (or the left eye
// alone when stereo is false), each eyeWidth by eyeHeight pixels.
std::vector<HologramVertex> BuildBodyHologram(const BodyOverlay& overlay, uint32_t eyeWidth, uint32_t eyeHeight,
                                              bool stereo);

// The overlay's field of view from a tracking packet's eye angles (left, right, up, down, radians).
void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4]);
