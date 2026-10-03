// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QPointF>
#include <QSet>

#include <cstdint>

#include <oxrsys/protocol/Protocol.h>

class QKeyEvent;

namespace oxrsys::qt_simulator
{

constexpr int LeftShiftKey = -1001;
constexpr int RightShiftKey = -1002;
// The middle mouse button is the headset button; the left one is the right trigger, or the left while F is held.
constexpr int HeadsetButtonKey = -1003;
constexpr int TriggerMouseKey = -1004;

struct SimulatorTrackingPose
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float headPosition[3] = {0.0f, 1.6f, 0.0f};
    // Hands rest at the sides of the body, relative to the head in its yaw frame, so they follow
    // walking and turning. While pointing, the right, dominant hand rises to just under the line of sight.
    float leftHandOffset[3] = {-0.22f, -0.72f, -0.05f};
    float rightHandOffset[3] = {0.22f, -0.72f, -0.05f};
    float pointingSeconds = 0.0f;
    float pointingAge = 0.0f;
};

void advanceSimulatorTracking(SimulatorTrackingPose& pose,
                              const QPointF& mouseDelta,
                              const QSet<int>& pressedKeys,
                              float deltaTime);

void fillSimulatorTrackingPacket(const SimulatorTrackingPose& pose,
                                 const QSet<int>& pressedKeys,
                                 int64_t timestampNs,
                                 float verticalFovDegrees,
                                 float eyeAspect,
                                 oxr::protocol::TrackingPacket& packet,
                                 bool controllersPresent = true);

int simulatorKeyIdentifier(const QKeyEvent& event);

} // namespace oxrsys::qt_simulator
