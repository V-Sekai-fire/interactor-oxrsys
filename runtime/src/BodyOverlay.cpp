// SPDX-License-Identifier: MPL-2.0

#include "BodyOverlay.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

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
    // Feet: a ring of 25 cm on the floor under the head and a cross at its centre.
    circle(b, feet, 0.25f, 48, Gold);
    b.segment({feet.x - 0.1f, feet.y, feet.z}, {feet.x + 0.1f, feet.y, feet.z}, Gold);
    b.segment({feet.x, feet.y, feet.z - 0.1f}, {feet.x, feet.y, feet.z + 0.1f}, Gold);
    // The play area's centre: a cross that turns green once the feet are within SnapMeters of it.
    const Colour& centre = FeetAtCentre(overlay) ? Green : Gold;
    circle(b, {0.0f, overlay.floorY, 0.0f}, 0.02f, 16, centre);
    b.segment({-0.04f, overlay.floorY, 0.0f}, {0.04f, overlay.floorY, 0.0f}, centre);
    b.segment({0.0f, overlay.floorY, -0.04f}, {0.0f, overlay.floorY, 0.04f}, centre);
    // xr-grid's procedural grid: a world-fixed lattice seen as a bubble around the feet and each hand,
    // a disc at each node and a thinner, fainter line to each neighbour, fading with distance.
    const int reach = int(std::ceil((XrGridFarFade + XrGridFadeZone) / XrGridStepMeters));
    std::vector<Vec3> foci = {feet};
    for (int hand = 0; hand < 2; ++hand)
    {
        if (overlay.handActive[hand])
            foci.push_back({overlay.handPosition[hand][0], overlay.handPosition[hand][1], overlay.handPosition[hand][2]});
    }
    std::set<std::tuple<int, int, int>> nodes;
    for (const Vec3& focus : foci)
    {
        const int fx = int(std::lround(focus.x / XrGridStepMeters));
        const int fy = int(std::lround((focus.y - overlay.floorY) / XrGridStepMeters));
        const int fz = int(std::lround(focus.z / XrGridStepMeters));
        for (int i = fx - reach; i <= fx + reach; ++i)
        {
            for (int j = std::max(0, fy - reach); j <= fy + reach; ++j)
            {
                for (int k = fz - reach; k <= fz + reach; ++k)
                    nodes.insert({i, j, k});
            }
        }
    }
    const float lineThickness = std::max(1.0f, b.thickness() * 0.5f);
    for (const std::tuple<int, int, int>& index : nodes)
    {
        const Vec3 node = {XrGridStepMeters * float(std::get<0>(index)),
                           overlay.floorY + XrGridStepMeters * float(std::get<1>(index)),
                           XrGridStepMeters * float(std::get<2>(index))};
        const float at[3] = {node.x, node.y, node.z};
        const float opacity = XrGridOpacity(overlay, at);
        if (opacity > 0.01f)
            b.disc(node, XrGridPointRadius, Faded(Gold, 0.85f * opacity));
        const Vec3 neighbours[3] = {{node.x + XrGridStepMeters, node.y, node.z},
                                    {node.x, node.y + XrGridStepMeters, node.z},
                                    {node.x, node.y, node.z + XrGridStepMeters}};
        for (const Vec3& next : neighbours)
        {
            const float to[3] = {next.x, next.y, next.z};
            const float nextOpacity = XrGridOpacity(overlay, to);
            if (opacity > 0.01f || nextOpacity > 0.01f)
                b.segment(node, next, Faded(Gold, 0.4f * opacity), Faded(Gold, 0.4f * nextOpacity), lineThickness);
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

bool FeetAtCentre(const BodyOverlay& overlay)
{
    const float x = overlay.headPosition[0];
    const float z = overlay.headPosition[2];
    return std::sqrt(x * x + z * z) < SnapMeters;
}

float XrGridOpacity(const BodyOverlay& overlay, const float point[3])
{
    const float feet[3] = {overlay.headPosition[0], overlay.floorY, overlay.headPosition[2]};
    const float* foci[3] = {feet, overlay.handPosition[0], overlay.handPosition[1]};
    const bool active[3] = {true, overlay.handActive[0], overlay.handActive[1]};
    float best = 0.0f;
    for (int f = 0; f < 3; ++f)
    {
        if (!active[f])
            continue;
        const float dx = point[0] - foci[f][0];
        const float dy = point[1] - foci[f][1];
        const float dz = point[2] - foci[f][2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float opacity = 1.0f;
        if (d > XrGridFarFade)
            opacity = std::clamp(1.0f + (XrGridFarFade - d) / XrGridFadeZone, 0.0f, 1.0f);
        best = std::max(best, opacity);
    }
    const float hx = point[0] - overlay.headPosition[0];
    const float hy = point[1] - overlay.headPosition[1];
    const float hz = point[2] - overlay.headPosition[2];
    const float fromEye = std::sqrt(hx * hx + hy * hy + hz * hz);
    if (fromEye < XrGridNearFade)
        best *= fromEye / XrGridNearFade;
    return best;
}

void SetEyeTangentsFromAngles(BodyOverlay& overlay, const float angles[4])
{
    for (int i = 0; i < 4; ++i)
        overlay.eyeTangents[i] = std::tan(angles[i]);
}
