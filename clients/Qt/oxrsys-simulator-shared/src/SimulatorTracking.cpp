// SPDX-License-Identifier: MPL-2.0

#include "SimulatorTracking.h"

#include <QKeyEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace
{

struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion multiply(const Quaternion& lhs, const Quaternion& rhs)
{
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

Quaternion axisAngle(float x, float y, float z, float angle)
{
    const float halfAngle = angle * 0.5f;
    const float sine = std::sin(halfAngle);
    return {x * sine, y * sine, z * sine, std::cos(halfAngle)};
}

struct Vector
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vector rotate(const Quaternion& q, const Vector& v)
{
    const Quaternion p = multiply(multiply(q, {v.x, v.y, v.z, 0.0f}), {-q.x, -q.y, -q.z, q.w});
    return {p.x, p.y, p.z};
}

Quaternion headQuaternion(float yaw, float pitch, float roll)
{
    return multiply(multiply(axisAngle(0.0f, 1.0f, 0.0f, yaw),
                            axisAngle(1.0f, 0.0f, 0.0f, pitch)),
                    axisAngle(0.0f, 0.0f, 1.0f, roll));
}

bool containsAny(const QSet<int>& keys, std::initializer_list<int> values)
{
    for (int value : values)
    {
        if (keys.contains(value))
        {
            return true;
        }
    }
    return false;
}

} // namespace

namespace oxrsys::qt_simulator
{

void advanceSimulatorTracking(SimulatorTrackingPose& pose,
                              const QPointF& mouseDelta,
                              const QSet<int>& pressedKeys,
                              float deltaTime)
{
    constexpr float MouseSensitivity = 0.003f;
    constexpr float MoveSpeed = 2.0f;

    // Pressing the trigger or the system button raises the right hand to point; it lowers 1.5 s later.
    if (containsAny(pressedKeys, {TriggerMouseKey, Qt::Key_H, Qt::Key_M}))
    {
        pose.pointingSeconds = 1.5f;
    }
    pose.pointingSeconds = std::max(0.0f, pose.pointingSeconds - deltaTime);

    pose.yaw -= static_cast<float>(mouseDelta.x()) * MouseSensitivity;
    pose.pitch -= static_cast<float>(mouseDelta.y()) * MouseSensitivity;
    pose.pitch = std::clamp(pose.pitch, -1.5f, 1.5f);

    if (pressedKeys.contains(Qt::Key_E))
    {
        pose.roll -= 1.5f * deltaTime;
    }
    if (pressedKeys.contains(Qt::Key_R))
    {
        pose.roll += 1.5f * deltaTime;
    }

    float forwardAmount = 0.0f;
    float strafeAmount = 0.0f;
    if (containsAny(pressedKeys, {Qt::Key_W, Qt::Key_Z}))
    {
        forwardAmount += 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_S))
    {
        forwardAmount -= 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_D))
    {
        strafeAmount += 1.0f;
    }
    if (containsAny(pressedKeys, {Qt::Key_A, Qt::Key_Q}))
    {
        strafeAmount -= 1.0f;
    }
    forwardAmount = std::clamp(forwardAmount, -1.0f, 1.0f);
    strafeAmount = std::clamp(strafeAmount, -1.0f, 1.0f);

    const float forwardX = -std::sin(pose.yaw);
    const float forwardZ = -std::cos(pose.yaw);
    const float rightX = std::cos(pose.yaw);
    const float rightZ = -std::sin(pose.yaw);
    float moveX = forwardX * forwardAmount + rightX * strafeAmount;
    float moveZ = forwardZ * forwardAmount + rightZ * strafeAmount;
    const float moveLength = std::sqrt(moveX * moveX + moveZ * moveZ);
    if (moveLength <= 0.001f)
    {
        return;
    }

    const float step = MoveSpeed * deltaTime / moveLength;
    const bool leftShift = pressedKeys.contains(LeftShiftKey);
    const bool rightShift = pressedKeys.contains(RightShiftKey);
    float* hand = leftShift && !rightShift ? pose.leftHandOffset : rightShift && !leftShift ? pose.rightHandOffset : nullptr;
    if (hand != nullptr)
    {
        hand[0] += strafeAmount * step;
        hand[2] -= forwardAmount * step;
        return;
    }
    pose.headPosition[0] += moveX * step;
    pose.headPosition[2] += moveZ * step;
}

void fillSimulatorTrackingPacket(const SimulatorTrackingPose& pose,
                                 const QSet<int>& pressedKeys,
                                 int64_t timestampNs,
                                 float verticalFovDegrees,
                                 float eyeAspect,
                                 oxr::protocol::TrackingPacket& packet,
                                 bool controllersPresent)
{
    packet = {};
    packet.timestampNs = timestampNs;
    packet.trackingFlags = controllersPresent ? (oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
                                                 oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE)
                                              : 0u;
    std::copy(std::begin(pose.headPosition),
              std::end(pose.headPosition),
              std::begin(packet.headPosition));

    const Quaternion orientation = headQuaternion(pose.yaw, pose.pitch, pose.roll);
    packet.headOrientation[0] = orientation.x;
    packet.headOrientation[1] = orientation.y;
    packet.headOrientation[2] = orientation.z;
    packet.headOrientation[3] = orientation.w;

    const Vector head = {pose.headPosition[0], pose.headPosition[1], pose.headPosition[2]};
    const Quaternion bodyYaw = axisAngle(0.0f, 1.0f, 0.0f, pose.yaw);
    const auto store = [](const Vector& p, const Quaternion& q, float* position, float* rotation) {
        position[0] = p.x;
        position[1] = p.y;
        position[2] = p.z;
        rotation[0] = q.x;
        rotation[1] = q.y;
        rotation[2] = q.z;
        rotation[3] = q.w;
    };

    const Vector left = rotate(bodyYaw, {pose.leftHandOffset[0], pose.leftHandOffset[1], pose.leftHandOffset[2]});
    store({head.x + left.x, head.y + left.y, head.z + left.z}, bodyYaw, packet.leftControllerPos,
          packet.leftControllerRot);
    if (pose.pointingSeconds <= 0.0f)
    {
        const Vector right = rotate(bodyYaw, {pose.rightHandOffset[0], pose.rightHandOffset[1], pose.rightHandOffset[2]});
        store({head.x + right.x, head.y + right.y, head.z + right.z}, bodyYaw, packet.rightControllerPos,
              packet.rightControllerRot);
    }
    else
    {
        // Arm IK sights the right hand along the gaze, as a person points at what they look at: the hand
        // sits on the line of sight at the reach the shoulder allows and points along it, so its laser
        // lands on the reticle at every distance.
        constexpr float ArmLength = 0.6f;
        constexpr float PreferredReach = 0.45f;
        const Vector g = rotate(orientation, {0.0f, 0.0f, -1.0f});
        const Vector s = rotate(bodyYaw, {pose.rightShoulder[0], pose.rightShoulder[1], pose.rightShoulder[2]});
        const Vector shoulder = {head.x + s.x, head.y + s.y, head.z + s.z};
        const Vector eyeToShoulder = {head.x - shoulder.x, head.y - shoulder.y, head.z - shoulder.z};
        const float b = g.x * eyeToShoulder.x + g.y * eyeToShoulder.y + g.z * eyeToShoulder.z;
        const float c = eyeToShoulder.x * eyeToShoulder.x + eyeToShoulder.y * eyeToShoulder.y +
                        eyeToShoulder.z * eyeToShoulder.z - ArmLength * ArmLength;
        const float discriminant = b * b - c;
        const float farthest = discriminant > 0.0f ? -b + std::sqrt(discriminant) : PreferredReach;
        const float reach = std::clamp(std::min(PreferredReach, farthest), 0.05f, PreferredReach);
        store({head.x + g.x * reach, head.y + g.y * reach, head.z + g.z * reach}, orientation,
              packet.rightControllerPos, packet.rightControllerRot);
    }

    if (pressedKeys.contains(Qt::Key_F))
    {
        packet.buttonState |= oxr::protocol::BUTTON_LEFT_GRIP;
        packet.leftGrip = 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_G))
    {
        packet.buttonState |= oxr::protocol::BUTTON_RIGHT_GRIP;
        packet.rightGrip = 1.0f;
    }
    if (pressedKeys.contains(HeadsetButtonKey))
    {
        packet.buttonState |= oxr::protocol::BUTTON_HEADSET_SYSTEM;
    }
    const bool leftHand = pressedKeys.contains(Qt::Key_F) && !pressedKeys.contains(Qt::Key_G);
    if (pressedKeys.contains(TriggerMouseKey) && leftHand)
    {
        packet.buttonState |= oxr::protocol::BUTTON_LEFT_TRIGGER;
        packet.leftTrigger = 1.0f;
    }
    if (pressedKeys.contains(TriggerMouseKey) && !leftHand)
    {
        packet.buttonState |= oxr::protocol::BUTTON_RIGHT_TRIGGER;
        packet.rightTrigger = 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_T))
    {
        packet.buttonState |= oxr::protocol::BUTTON_LEFT_TRIGGER;
        packet.leftTrigger = 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_H))
    {
        packet.buttonState |= oxr::protocol::BUTTON_RIGHT_TRIGGER;
        packet.rightTrigger = 1.0f;
    }

    const std::pair<int, uint32_t> buttons[] = {
        {Qt::Key_1, oxr::protocol::BUTTON_X},
        {Qt::Key_2, oxr::protocol::BUTTON_Y},
        {Qt::Key_3, oxr::protocol::BUTTON_A},
        {Qt::Key_4, oxr::protocol::BUTTON_B},
        {Qt::Key_M, oxr::protocol::BUTTON_MENU},
        {Qt::Key_C, oxr::protocol::BUTTON_LEFT_THUMBSTICK},
        {Qt::Key_N, oxr::protocol::BUTTON_RIGHT_THUMBSTICK},
    };
    for (const auto& [key, bit] : buttons)
    {
        if (pressedKeys.contains(key))
        {
            packet.buttonState |= bit;
        }
    }

    const auto axis = [&pressedKeys](int negative, int positive) {
        return (pressedKeys.contains(positive) ? 1.0f : 0.0f) - (pressedKeys.contains(negative) ? 1.0f : 0.0f);
    };
    packet.leftThumbstick[0] = axis(Qt::Key_J, Qt::Key_L);
    packet.leftThumbstick[1] = axis(Qt::Key_K, Qt::Key_I);
    packet.rightThumbstick[0] = axis(Qt::Key_Left, Qt::Key_Right);
    packet.rightThumbstick[1] = axis(Qt::Key_Down, Qt::Key_Up);
    packet.ipd = 0.064f;

    const float clampedFovDegrees = std::clamp(verticalFovDegrees, 60.0f, 150.0f);
    const float clampedAspect = std::max(eyeAspect, 0.1f);
    constexpr float DegreesToRadians = 0.017453292519943295f;
    const float halfAngleV = clampedFovDegrees * 0.5f * DegreesToRadians;
    const float halfAngleH = std::atan(std::tan(halfAngleV) * clampedAspect);
    packet.eyeFov[0] = -halfAngleH;
    packet.eyeFov[1] = halfAngleH;
    packet.eyeFov[2] = halfAngleV;
    packet.eyeFov[3] = -halfAngleV;
}

int simulatorKeyIdentifier(const QKeyEvent& event)
{
    if (event.key() != Qt::Key_Shift)
    {
        return event.key();
    }

#if defined(Q_OS_MACOS)
    if (event.nativeVirtualKey() == 60)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#elif defined(Q_OS_WIN)
    if (event.nativeVirtualKey() == 0xA1)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#else
    if (event.nativeScanCode() == 54)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#endif
}

} // namespace oxrsys::qt_simulator
