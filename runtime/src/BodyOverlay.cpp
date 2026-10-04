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

} // namespace

bool ProjectToEyeDepth(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                       float& px, float& py, float& depth);

namespace
{

Colour Faded(const Colour& c, float alpha)
{
    return {c.r, c.g, c.b, alpha};
}

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

    void segment(const Vec3& a, const Vec3& b, const Colour& colour) { segment(a, b, colour, colour, thickness_); }

    // A segment whose colour runs from ca at a to cb at b.
    void segment(const Vec3& a, const Vec3& b, const Colour& ca, const Colour& cb, float thickness)
    {
        for (int eye = 0; eye < eyes_; ++eye)
        {
            float ax, ay, bx, by;
            const float pa[3] = {a.x, a.y, a.z};
            const float pb[3] = {b.x, b.y, b.z};
            if (!ProjectToEye(overlay_, eye, pa, w_, h_, ax, ay) || !ProjectToEye(overlay_, eye, pb, w_, h_, bx, by))
                continue;
            if (!nearView(ax, ay) || !nearView(bx, by))
                continue;
            quad(eye, ax, ay, bx, by, ca, cb, thickness);
        }
    }

    // A filled disc facing the eye, radius metres across at its distance, as xr-grid draws a node.
    void disc(const Vec3& centre, float radius, const Colour& colour)
    {
        const float pc[3] = {centre.x, centre.y, centre.z};
        for (int eye = 0; eye < eyes_; ++eye)
        {
            float cx, cy, depth;
            if (!ProjectToEyeDepth(overlay_, eye, pc, w_, h_, cx, cy, depth) || !nearView(cx, cy))
                continue;
            const float span = overlay_.eyeTangents[1] - overlay_.eyeTangents[0];
            const float r = std::max(1.0f, radius / depth / span * w_);
            constexpr int Sides = 8;
            for (int i = 0; i < Sides; ++i)
            {
                const float t0 = 6.2831853f * float(i) / float(Sides);
                const float t1 = 6.2831853f * float(i + 1) / float(Sides);
                vertex(eye, cx, cy, colour);
                vertex(eye, cx + r * std::cos(t0), cy + r * std::sin(t0), colour);
                vertex(eye, cx + r * std::cos(t1), cy + r * std::sin(t1), colour);
            }
        }
    }

    float thickness() const { return thickness_; }

    std::vector<HologramVertex> take() { return std::move(out_); }

private:
    // Within 0.45 of an eye of the view, so geometry beside the eye does not stretch across it.
    bool nearView(float x, float y) const
    {
        return x > -0.45f * w_ && x < 1.45f * w_ && y > -0.45f * h_ && y < 1.45f * h_;
    }

    void quad(int eye, float ax, float ay, float bx, float by, const Colour& ca, const Colour& cb, float thickness)
    {
        const float dx = bx - ax;
        const float dy = by - ay;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-3f || length > 4.0f * (w_ + h_))
            return;
        const float nx = -dy / length * thickness * 0.5f;
        const float ny = dx / length * thickness * 0.5f;
        const float corners[4][2] = {{ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}, {ax - nx, ay - ny}};
        const bool atB[4] = {false, true, true, false};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i : order)
            vertex(eye, corners[i][0], corners[i][1], atB[i] ? cb : ca);
    }

    void vertex(int eye, float x, float y, const Colour& c)
    {
        HologramVertex v;
        v.x = (x + float(eye) * w_) / (w_ * float(eyes_)) * 2.0f - 1.0f;
        v.y = 1.0f - y / h_ * 2.0f;
        v.r = c.r;
        v.g = c.g;
        v.b = c.b;
        v.a = c.a;
        out_.push_back(v);
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
    float depth = 0.0f;
    return ProjectToEyeDepth(overlay, eye, world, eyeWidth, eyeHeight, px, py, depth);
}

bool ProjectToEyeDepth(const BodyOverlay& overlay, int eye, const float world[3], float eyeWidth, float eyeHeight,
                       float& px, float& py, float& depth)
{
    const float half = overlay.ipd * 0.5f;
    const Vec3 offset = rotate(overlay.headOrientation, {eye == 0 ? -half : half, 0.0f, 0.0f});
    const Vec3 eyePosition = {overlay.headPosition[0] + offset.x, overlay.headPosition[1] + offset.y,
                              overlay.headPosition[2] + offset.z};
    const Vec3 v = inverseRotate(overlay.headOrientation,
                                 {world[0] - eyePosition.x, world[1] - eyePosition.y, world[2] - eyePosition.z});
    if (v.z > -0.05f)
        return false;
    depth = -v.z;
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
    // Kept sparse as CASSIE keeps its interface: a thin ring at the feet, a faint sphere at the play
    // area's centre that turns green once the feet are within SnapMeters, a small sphere per hand,
    // and xr-grid's floor grid around the feet only.
    circle(b, feet, 0.25f, 48, Gold);
    b.disc({0.0f, overlay.floorY, 0.0f}, 0.015f, FeetAtCentre(overlay) ? Green : FaintGold);
    if (overlay.bodyActive)
    {
        // Bones as joint pairs, in the protocol's joint order; the head is the eye, so it is not drawn.
        constexpr int Bones[][2] = {{0, 1},   {1, 2},   {2, 4},   {4, 5},   {5, 6},   {2, 7},   {7, 8},
                                    {8, 9},   {0, 10},  {10, 11}, {11, 12}, {12, 13}, {0, 14}, {14, 15},
                                    {15, 16}, {16, 17}};
        const float boneThickness = std::max(1.0f, b.thickness() * 0.75f);
        for (const int* bone : Bones)
        {
            const float* p = overlay.body[bone[0]];
            const float* q = overlay.body[bone[1]];
            b.segment({p[0], p[1], p[2]}, {q[0], q[1], q[2]}, FaintGold, FaintGold, boneThickness);
        }
        for (int joint = 0; joint < BodyJointCount; ++joint)
        {
            if (joint != 3)
            {
                const float* p = overlay.body[joint];
                b.disc({p[0], p[1], p[2]}, 0.008f, FaintGold);
            }
        }
        constexpr int Ankles[2] = {12, 16};
        for (int foot = 0; foot < 2; ++foot)
        {
            const float* a = overlay.body[Ankles[foot]];
            const bool planted = (overlay.bodyContact & (1u << foot)) != 0;
            b.disc({a[0], overlay.floorY, a[2]}, planted ? 0.03f : 0.015f, planted ? Gold : FaintGold);
        }
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        if (overlay.handActive[hand])
        {
            const float* p = overlay.handPosition[hand];
            b.disc({p[0], p[1], p[2]}, 0.012f, Gold);
        }
    }
    const int reach = int(std::ceil((XrGridFarFade + XrGridFadeZone) / XrGridStepMeters));
    const int fx = int(std::lround(feet.x / XrGridStepMeters));
    const int fz = int(std::lround(feet.z / XrGridStepMeters));
    const float lineThickness = std::max(1.0f, b.thickness() * 0.5f);
    for (int i = fx - reach; i <= fx + reach; ++i)
    {
        for (int k = fz - reach; k <= fz + reach; ++k)
        {
            const Vec3 node = {XrGridStepMeters * float(i), overlay.floorY, XrGridStepMeters * float(k)};
            const float at[3] = {node.x, node.y, node.z};
            const float opacity = XrGridOpacity(overlay, at);
            if (opacity > 0.01f)
                b.disc(node, XrGridPointRadius, Faded(Gold, 0.7f * opacity));
            const Vec3 neighbours[2] = {{node.x + XrGridStepMeters, node.y, node.z},
                                        {node.x, node.y, node.z + XrGridStepMeters}};
            for (const Vec3& next : neighbours)
            {
                const float to[3] = {next.x, next.y, next.z};
                const float nextOpacity = XrGridOpacity(overlay, to);
                if (opacity > 0.01f || nextOpacity > 0.01f)
                    b.segment(node, next, Faded(Gold, 0.25f * opacity), Faded(Gold, 0.25f * nextOpacity),
                              lineThickness);
            }
        }
    }
    return b.take();
}

bool FeetAtCentre(const BodyOverlay& overlay)
{
    const float x = overlay.headPosition[0];
    const float z = overlay.headPosition[2];
    return std::sqrt(x * x + z * z) < SnapMeters;
}

float XrGridOpacity(const BodyOverlay& overlay, const float point[3])
{
    const float dx = point[0] - overlay.headPosition[0];
    const float dy = point[1] - overlay.floorY;
    const float dz = point[2] - overlay.headPosition[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d <= XrGridFarFade)
        return 1.0f;
    return std::clamp(1.0f + (XrGridFarFade - d) / XrGridFadeZone, 0.0f, 1.0f);
}

void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4])
{
    for (int i = 0; i < 4; ++i)
        overlay.eyeTangents[i] = std::tan(angles[i]);
}
