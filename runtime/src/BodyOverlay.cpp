// SPDX-License-Identifier: MPL-2.0

#include "BodyOverlay.h"

#include <algorithm>
#include <cmath>

namespace
{

struct Vec3
{
    float x, y, z;
};

Vec3 rotate(const float q[4], const Vec3& v)
{
    // v + 2 w (u x v) + 2 u x (u x v), with u the quaternion's vector part.
    const Vec3 u = {q[0], q[1], q[2]};
    const float w = q[3];
    const Vec3 c1 = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const Vec3 c2 = {u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
    return {v.x + 2.0f * (w * c1.x + c2.x), v.y + 2.0f * (w * c1.y + c2.y), v.z + 2.0f * (w * c1.z + c2.z)};
}

Vec3 inverseRotate(const float q[4], const Vec3& v)
{
    const float conjugate[4] = {-q[0], -q[1], -q[2], q[3]};
    return rotate(conjugate, v);
}

struct Colour
{
    float r, g, b, a;
};

constexpr Colour Gold = {1.0f, 0.78f, 0.24f, 0.85f};
constexpr Colour FaintGold = {1.0f, 0.78f, 0.24f, 0.35f};
constexpr Colour Green = {0.35f, 1.0f, 0.5f, 0.9f};

class Builder
{
public:
    Builder(const BodyOverlay& overlay, uint32_t eyeWidth, uint32_t eyeHeight, bool stereo)
        : overlay_(overlay)
        , w_(float(eyeWidth))
        , h_(float(eyeHeight))
        , eyes_(stereo ? 2 : 1)
    {
        thickness_ = std::max(2.0f, h_ / 400.0f);
    }

    // A closed polyline in world space, drawn as thick pixel-space segments in each eye.
    void loop(const Vec3* points, int count, const Colour& colour)
    {
        for (int i = 0; i < count; ++i)
            segment(points[i], points[(i + 1) % count], colour);
    }

    void segment(const Vec3& a, const Vec3& b, const Colour& colour)
    {
        for (int eye = 0; eye < eyes_; ++eye)
        {
            float ax, ay, bx, by;
            const float pa[3] = {a.x, a.y, a.z};
            const float pb[3] = {b.x, b.y, b.z};
            if (!ProjectToEye(overlay_, eye, pa, w_, h_, ax, ay) || !ProjectToEye(overlay_, eye, pb, w_, h_, bx, by))
                continue;
            quad(eye, ax, ay, bx, by, colour);
        }
    }

    std::vector<HologramVertex> take() { return std::move(out_); }

private:
    void quad(int eye, float ax, float ay, float bx, float by, const Colour& c)
    {
        const float dx = bx - ax;
        const float dy = by - ay;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-3f || length > 4.0f * (w_ + h_))
            return;
        const float nx = -dy / length * thickness_ * 0.5f;
        const float ny = dx / length * thickness_ * 0.5f;
        const float corners[4][2] = {{ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}, {ax - nx, ay - ny}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        const float totalWidth = w_ * float(eyes_);
        for (int i : order)
        {
            HologramVertex v;
            v.x = (corners[i][0] + float(eye) * w_) / totalWidth * 2.0f - 1.0f;
            v.y = 1.0f - corners[i][1] / h_ * 2.0f;
            v.r = c.r;
            v.g = c.g;
            v.b = c.b;
            v.a = c.a;
            out_.push_back(v);
        }
    }

    const BodyOverlay& overlay_;
    float w_;
    float h_;
    int eyes_;
    float thickness_ = 2.0f;
    std::vector<HologramVertex> out_;
};

// A horizontal circle of radius r around centre.
void circle(Builder& b, const Vec3& centre, float r, int segments, const Colour& colour)
{
    std::vector<Vec3> points;
    for (int i = 0; i < segments; ++i)
    {
        const float t = 6.2831853f * float(i) / float(segments);
        points.push_back({centre.x + r * std::cos(t), centre.y, centre.z + r * std::sin(t)});
    }
    b.loop(points.data(), int(points.size()), colour);
}

} // namespace

bool ProjectToEye(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                  float& px, float& py)
{
    const float half = overlay.ipd * 0.5f;
    const Vec3 offset = rotate(overlay.headOrientation, {eye == 0 ? -half : half, 0.0f, 0.0f});
    const Vec3 eyePosition = {overlay.headPosition[0] + offset.x, overlay.headPosition[1] + offset.y,
                              overlay.headPosition[2] + offset.z};
    const Vec3 v = inverseRotate(overlay.headOrientation,
                                 {world[0] - eyePosition.x, world[1] - eyePosition.y, world[2] - eyePosition.z});
    if (v.z > -0.05f)
        return false;
    const float tx = v.x / -v.z;
    const float ty = v.y / -v.z;
    float left = overlay.eyeTangents[0];
    float right = overlay.eyeTangents[1];
    if (eye == 1)
    {
        left = -overlay.eyeTangents[1];
        right = -overlay.eyeTangents[0];
    }
    const float up = overlay.eyeTangents[2];
    const float down = overlay.eyeTangents[3];
    if (right - left <= 0.0f || up - down <= 0.0f)
        return false;
    px = (tx - left) / (right - left) * eyeWidth;
    py = (up - ty) / (up - down) * eyeHeight;
    return true;
}

std::vector<HologramVertex> BuildBodyHologram(const BodyOverlay& overlay, uint32_t eyeWidth, uint32_t eyeHeight,
                                              bool stereo)
{
    if (!overlay.enabled || eyeWidth == 0 || eyeHeight == 0)
        return {};
    Builder b(overlay, eyeWidth, eyeHeight, stereo);
    const Vec3 feet = {overlay.headPosition[0], overlay.floorY, overlay.headPosition[2]};
    // Feet: a ring of 25 cm on the floor under the head and a cross at its centre.
    circle(b, feet, 0.25f, 48, Gold);
    b.segment({feet.x - 0.1f, feet.y, feet.z}, {feet.x + 0.1f, feet.y, feet.z}, Gold);
    b.segment({feet.x, feet.y, feet.z - 0.1f}, {feet.x, feet.y, feet.z + 0.1f}, Gold);
    // A floor grid around the play area's centre, a scale reference as in CASSIE: a node every
    // GridStepMeters, the one nearest the feet lit, and the centre green once the feet are within
    // SnapMeters of it, so a shift of one step reads as the light moving to the next node.
    int nearX = 0;
    int nearZ = 0;
    NearestGridNode(overlay, nearX, nearZ);
    const bool atCentre = FeetAtCentre(overlay);
    for (int i = -GridHalfNodes; i <= GridHalfNodes; ++i)
    {
        for (int j = -GridHalfNodes; j <= GridHalfNodes; ++j)
        {
            const Vec3 node = {GridStepMeters * float(i), overlay.floorY, GridStepMeters * float(j)};
            if (i == 0 && j == 0)
            {
                const Colour& centre = atCentre ? Green : Gold;
                circle(b, node, 0.02f, 16, centre);
                b.segment({node.x - 0.04f, node.y, node.z}, {node.x + 0.04f, node.y, node.z}, centre);
                b.segment({node.x, node.y, node.z - 0.04f}, {node.x, node.y, node.z + 0.04f}, centre);
            }
            else if (i == nearX && j == nearZ)
            {
                circle(b, node, 0.012f, 12, Gold);
            }
            else
            {
                b.segment({node.x - 0.006f, node.y, node.z}, {node.x + 0.006f, node.y, node.z}, FaintGold);
            }
        }
    }
    // Head height: a faint ring of 60 cm around the head.
    circle(b, {overlay.headPosition[0], overlay.headPosition[1], overlay.headPosition[2]}, 0.6f, 64, FaintGold);
    for (int hand = 0; hand < 2; ++hand)
    {
        if (!overlay.handActive[hand])
            continue;
        const float* p = overlay.handPosition[hand];
        circle(b, {p[0], p[1], p[2]}, 0.05f, 16, Gold);
    }
    return b.take();
}

void NearestGridNode(const BodyOverlay& overlay, int& x, int& z)
{
    x = int(std::lround(overlay.headPosition[0] / GridStepMeters));
    z = int(std::lround(overlay.headPosition[2] / GridStepMeters));
}

bool FeetAtCentre(const BodyOverlay& overlay)
{
    const float x = overlay.headPosition[0];
    const float z = overlay.headPosition[2];
    return std::sqrt(x * x + z * z) < SnapMeters;
}

void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4])
{
    for (int i = 0; i < 4; ++i)
        overlay.eyeTangents[i] = std::tan(angles[i]);
}
