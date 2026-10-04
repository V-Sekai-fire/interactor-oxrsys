// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "BodyOverlay.h"

#include <cmath>

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

TEST_CASE("Body hologram's grid lights the node nearest the feet and the centre within 4 cm", "[BodyOverlay]")
{
    BodyOverlay overlay = LookingDown();
    int x = 99;
    int z = 99;
    overlay.headPosition[0] = 0.024f;
    NearestGridNode(overlay, x, z);
    CHECK(x == 0);
    CHECK(z == 0);
    CHECK(FeetAtCentre(overlay));

    // Two centimetres further, the light steps to the next node though the feet barely moved.
    overlay.headPosition[0] = 0.026f;
    NearestGridNode(overlay, x, z);
    CHECK(x == 1);
    CHECK(FeetAtCentre(overlay));

    overlay.headPosition[0] = 0.0f;
    overlay.headPosition[2] = -0.11f;
    NearestGridNode(overlay, x, z);
    CHECK(x == 0);
    CHECK(z == -2);
    CHECK_FALSE(FeetAtCentre(overlay));

    // Control: the centre is gold 5 cm away and green 3 cm away.
    overlay.headPosition[2] = 0.05f;
    CHECK_FALSE(HasGreen(BuildBodyHologram(overlay, 1000, 1000, true)));
    overlay.headPosition[2] = 0.03f;
    CHECK(HasGreen(BuildBodyHologram(overlay, 1000, 1000, true)));
}
