// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "BodyOverlay.h"

#include <cmath>
#include <cstring>

using Catch::Matchers::WithinAbs;

namespace
{

constexpr float EyeWidth = 1000.0f;
constexpr float EyeHeight = 1000.0f;

// A head at 1.6 m looking straight down, with a 90 degree field of view and no IPD.
BodyOverlay LookingDown()
{
    BodyOverlay overlay;
    overlay.enabled = true;
    overlay.ipd = 0.0f;
    const float half = -0.5f * 1.5707963f; // pitch -90 degrees about +X
    overlay.headOrientation[0] = std::sin(half);
    overlay.headOrientation[3] = std::cos(half);
    return overlay;
}

// The on-screen radius of the floor ring point 0.25 m to the side of the feet.
float FloorRingRadius(const BodyOverlay& overlay)
{
    const float point[3] = {overlay.headPosition[0] + 0.25f, overlay.floorY, overlay.headPosition[2]};
    float px = 0.0f;
    float py = 0.0f;
    REQUIRE(ProjectToEye(overlay, 0, point, EyeWidth, EyeHeight, px, py));
    return std::abs(px - EyeWidth * 0.5f);
}

bool HasGreen(const std::vector<HologramVertex>& vertices)
{
    for (const HologramVertex& v : vertices)
    {
        if (v.r < 0.5f && v.g > 0.9f)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Body hologram projects a point straight ahead to the eye's centre", "[BodyOverlay]")
{
    BodyOverlay overlay;
    overlay.enabled = true;
    overlay.ipd = 0.0f;
    const float ahead[3] = {0.0f, 1.6f, -2.0f};
    float px = 0.0f;
    float py = 0.0f;
    REQUIRE(ProjectToEye(overlay, 0, ahead, EyeWidth, EyeHeight, px, py));
    CHECK_THAT(px, WithinAbs(500.0, 0.01));
    CHECK_THAT(py, WithinAbs(500.0, 0.01));

    const float behind[3] = {0.0f, 1.6f, 2.0f};
    CHECK_FALSE(ProjectToEye(overlay, 0, behind, EyeWidth, EyeHeight, px, py));
}

TEST_CASE("Body hologram's floor ring shrinks with the head's height above the floor", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    // With tangents of 1, a point r metres aside at depth h sits r / h of the half-width from centre.
    CHECK_THAT(FloorRingRadius(overlay), WithinAbs(0.25 / 1.6 * 500.0, 0.05));

    // Control: a floor counted twice, 1.6 m lower, draws the ring at half that size, which the
    // check above would catch.
    BodyOverlay doubled = overlay;
    doubled.floorY = -1.6f;
    CHECK(std::abs(FloorRingRadius(doubled) - FloorRingRadius(overlay)) > 10.0f);
}

TEST_CASE("Body hologram mirrors the right eye's field and offsets it by the IPD", "[BodyOverlay]")
{
    BodyOverlay overlay;
    overlay.enabled = true;
    overlay.ipd = 0.064f;
    overlay.eyeTangents[0] = -1.2f;
    overlay.eyeTangents[1] = 0.8f;
    const float ahead[3] = {0.0f, 1.6f, -1.0f};
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    REQUIRE(ProjectToEye(overlay, 0, ahead, EyeWidth, EyeHeight, lx, ly));
    REQUIRE(ProjectToEye(overlay, 1, ahead, EyeWidth, EyeHeight, rx, ry));
    // Asymmetric fields mirror, so a centred point lands mirrored about each eye's middle.
    CHECK_THAT(lx + rx, WithinAbs(EyeWidth, 0.5));
    CHECK_THAT(ly, WithinAbs(ry, 0.01));
}

TEST_CASE("Body hologram is empty when disabled and grows with tracked hands", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    overlay.enabled = false;
    CHECK(BuildBodyHologram(overlay, 1000, 1000, true).empty());

    overlay.enabled = true;
    const size_t withoutHands = BuildBodyHologram(overlay, 1000, 1000, true).size();
    CHECK(withoutHands > 0);
    CHECK(withoutHands % 3 == 0);
    overlay.handActive[1] = true;
    overlay.handPosition[1][0] = 0.2f;
    overlay.handPosition[1][1] = 0.9f;
    CHECK(BuildBodyHologram(overlay, 1000, 1000, true).size() > withoutHands);

    for (const HologramVertex& v : BuildBodyHologram(overlay, 1000, 1000, true))
    {
        CHECK(v.x >= -1.5f);
        CHECK(v.x <= 1.5f);
    }
}

// A standing body 1.2 m ahead, its right foot planted.
void StandAhead(BodyOverlay& overlay)
{
    const float joints[BodyJointCount][3] = {
        {0.0f, 0.95f, -1.2f},  {0.0f, 1.2f, -1.2f},   {0.0f, 1.45f, -1.2f},  {0.0f, 1.6f, -1.2f},
        {-0.18f, 1.4f, -1.2f}, {-0.25f, 1.15f, -1.2f}, {-0.3f, 0.9f, -1.2f},  {0.18f, 1.4f, -1.2f},
        {0.25f, 1.15f, -1.2f}, {0.3f, 0.9f, -1.2f},    {-0.1f, 0.9f, -1.2f},  {-0.1f, 0.5f, -1.1f},
        {-0.1f, 0.15f, -1.2f}, {-0.1f, 0.05f, -1.4f}, {0.1f, 0.9f, -1.2f},   {0.1f, 0.48f, -1.2f},
        {0.1f, 0.07f, -1.2f},  {0.1f, 0.0f, -1.4f}};
    memcpy(overlay.body, joints, sizeof(joints));
    overlay.bodyActive = true;
    overlay.bodyContact = 0x02;
}

bool HasVertexNear(const std::vector<HologramVertex>& vertices, float x, float y, float r)
{
    for (const HologramVertex& v : vertices)
    {
        if (std::abs(v.x - x) < r && std::abs(v.y - y) < r)
            return true;
    }
    return false;
}

TEST_CASE("Body hologram draws the client's body and plants its feet on the floor", "[BodyOverlay]")
{
    BodyOverlay overlay;
    overlay.enabled = true;
    overlay.ipd = 0.0f;
    const size_t without = BuildBodyHologram(overlay, 1000, 1000, false).size();
    StandAhead(overlay);
    const std::vector<HologramVertex> with = BuildBodyHologram(overlay, 1000, 1000, false);
    CHECK(with.size() > without + 16 * 6);

    // The planted right foot's disc sits on the floor under its ankle.
    const float floorUnderAnkle[3] = {overlay.body[16][0], overlay.floorY, overlay.body[16][2]};
    float px = 0.0f;
    float py = 0.0f;
    REQUIRE(ProjectToEye(overlay, 0, floorUnderAnkle, 1000.0f, 1000.0f, px, py));
    CHECK(HasVertexNear(with, px / 1000.0f * 2.0f - 1.0f, 1.0f - py / 1000.0f * 2.0f, 0.002f));

    // Control: the body is only drawn while the client sends one.
    overlay.bodyActive = false;
    CHECK(BuildBodyHologram(overlay, 1000, 1000, false).size() == without);
}

TEST_CASE("Body hologram's centre turns green with the feet within 4 cm", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    overlay.headPosition[0] = 0.03f;
    CHECK(FeetAtCentre(overlay));
    CHECK(HasGreen(BuildBodyHologram(overlay, 1000, 1000, true)));
    // Control: 5 cm away the centre stays gold.
    overlay.headPosition[0] = 0.05f;
    CHECK_FALSE(FeetAtCentre(overlay));
    CHECK_FALSE(HasGreen(BuildBodyHologram(overlay, 1000, 1000, true)));
}

TEST_CASE("Body hologram's grid fades with distance from the feet as xr-grid's does", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    const float atFeet[3] = {0.1f, 0.0f, 0.0f};
    CHECK_THAT(XrGridOpacity(overlay, atFeet), WithinAbs(1.0, 1e-5));
    const float halfway[3] = {XrGridFarFade + 0.5f * XrGridFadeZone, 0.0f, 0.0f};
    CHECK_THAT(XrGridOpacity(overlay, halfway), WithinAbs(0.5, 1e-4));
    const float beyond[3] = {XrGridFarFade + XrGridFadeZone + 0.01f, 0.0f, 0.0f};
    CHECK_THAT(XrGridOpacity(overlay, beyond), WithinAbs(0.0, 1e-6));

    // Control: a tracked hand brings no grid of its own.
    overlay.handActive[1] = true;
    overlay.handPosition[1][0] = 0.6f;
    overlay.handPosition[1][1] = 1.0f;
    overlay.handPosition[1][2] = -0.3f;
    const float byHand[3] = {0.6f, 1.0f, -0.3f};
    CHECK_THAT(XrGridOpacity(overlay, byHand), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Body hologram's grid bubble moves across a fixed lattice with the feet", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    const size_t still = BuildBodyHologram(overlay, 1000, 1000, false).size();
    CHECK(still > 0);
    // Moving the feet half a step moves the bubble across the fixed lattice, which changes what is drawn.
    overlay.headPosition[0] = 0.05f;
    CHECK(BuildBodyHologram(overlay, 1000, 1000, false).size() != still);
}
