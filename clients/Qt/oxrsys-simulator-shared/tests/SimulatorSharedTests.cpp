// SPDX-License-Identifier: MPL-2.0

#include "SimulatorTracking.h"
#include "VideoFrameAssembler.h"

#include <oxrsys/protocol/FecCodec.h>

#include <QByteArray>
#include <QDebug>
#include <QSet>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

oxr::protocol::VideoPacketHeader videoHeader(uint32_t frameIndex,
                                             uint16_t packetIndex,
                                             uint16_t totalPackets,
                                             uint16_t payloadSize,
                                             uint8_t flags = oxr::protocol::VIDEO_FLAG_STEREO,
                                             uint16_t fecGroupLastPacketPayloadSize = 0)
{
    oxr::protocol::VideoPacketHeader header = {};
    header.frameIndex = frameIndex;
    header.packetIndex = packetIndex;
    header.totalPackets = totalPackets;
    header.payloadSize = payloadSize;
    header.flags = flags;
    header.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
    header.fecGroupLastPacketPayloadSize = fecGroupLastPacketPayloadSize;
    header.presentationTimeNs = 1234 + frameIndex;
    return header;
}

void testCompleteVideoFrame()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    const QByteArray second("de", 2);

    expect(assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10).isEmpty(),
           "Expected first packet to wait for frame completion");
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(1, 1, 2, 2), second.constData(), second.size(), 20);

    expect(frames.size() == 1, "Expected one complete frame");
    expect(frames.first().nalUnit == QByteArray("abcde", 5), "Expected packet payload concatenation");
    expect(frames.first().presentationTimeNs == 1235, "Expected presentation timestamp");
}

void testDuplicatePacketIgnored()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    const QByteArray duplicate("xxx", 3);
    const QByteArray second("de", 2);

    assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(1, 0, 2, 3), duplicate.constData(), duplicate.size(), 11);
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(1, 1, 2, 2), second.constData(), second.size(), 20);

    expect(frames.size() == 1, "Expected duplicate to keep one complete frame");
    expect(frames.first().nalUnit.startsWith("abc"), "Expected duplicate packet to be ignored");
}

void testIncompleteFrameDrop()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(2, 0, 1, 3), first.constData(), first.size(), 20);
    expect(assembler.droppedFrames() == 1, "Expected incomplete previous frame to drop");
}

void testRenderPoseIgnored()
{
    VideoFrameAssembler assembler;
    const QByteArray payload("pose", 4);
    oxr::protocol::VideoPacketHeader header = videoHeader(
        1,
        0,
        0,
        static_cast<uint16_t>(payload.size()),
        oxr::protocol::VIDEO_FLAG_RENDER_POSE);
    expect(assembler.addPacket(header, payload.constData(), payload.size(), 10).isEmpty(),
           "Expected render pose metadata to be ignored");
    expect(assembler.droppedFrames() == 0, "Expected render pose to not count as a dropped frame");
}

void testFecRecovery()
{
    VideoFrameAssembler assembler;
    QByteArray first("aaaa", 4);
    QByteArray missing("bbbb", 4);
    QByteArray third("cccc", 4);
    const uint8_t* payloads[] = {
        reinterpret_cast<const uint8_t*>(first.constData()),
        reinterpret_cast<const uint8_t*>(missing.constData()),
        reinterpret_cast<const uint8_t*>(third.constData()),
    };
    const uint16_t payloadSizes[] = {4, 4, 4};
    QByteArray fec;
    fec.resize(static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));
    oxr::fec::Encode(payloads, payloadSizes, 3, reinterpret_cast<uint8_t*>(fec.data()));

    assembler.addPacket(videoHeader(7, 0, 3, 4), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(7, 2, 3, 4), third.constData(), third.size(), 12);
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(7,
                                        0,
                                        3,
                                        static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD),
                                        oxr::protocol::VIDEO_FLAG_FEC |
                                            oxr::protocol::VIDEO_FLAG_STEREO),
                            fec.constData(),
                            fec.size(),
                            13);

    expect(frames.size() == 1, "Expected FEC to complete the frame");
    expect(frames.first().recoveredWithFec, "Expected frame to be marked as FEC recovered");
    expect(assembler.fecRecoveries() == 1, "Expected one FEC recovery");
    expect(frames.first().nalUnit.mid(4, 4) == missing,
           "Expected recovered packet bytes at the missing packet offset");
}

void testTrailingFecAfterCompleteFrameIgnored()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    const QByteArray fec(static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD), char(0));
    const uint8_t fecFlags = oxr::protocol::VIDEO_FLAG_FEC | oxr::protocol::VIDEO_FLAG_STEREO;
    const uint16_t fecSize = static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD);

    assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10);
    expect(assembler.addPacket(videoHeader(1, 1, 2, 3), first.constData(), first.size(), 11).size() == 1,
           "Expected the two-packet frame to complete");
    assembler.addPacket(videoHeader(1, 0, 2, fecSize, fecFlags), fec.constData(), fec.size(), 12);
    assembler.addPacket(videoHeader(2, 0, 2, 3), first.constData(), first.size(), 20);
    expect(assembler.addPacket(videoHeader(2, 1, 2, 3), first.constData(), first.size(), 21).size() == 1,
           "Expected the next frame to complete");
    expect(assembler.droppedFrames() == 0, "Expected trailing parity not to count a drop");
}

void testFecRecoveryUsesFinalShortPacketSize()
{
    VideoFrameAssembler assembler;
    QByteArray first(static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD), 'a');
    QByteArray missing("tail", 4);
    const uint8_t* payloads[] = {
        reinterpret_cast<const uint8_t*>(first.constData()),
        reinterpret_cast<const uint8_t*>(missing.constData()),
    };
    const uint16_t payloadSizes[] = {
        static_cast<uint16_t>(first.size()),
        static_cast<uint16_t>(missing.size()),
    };
    QByteArray fec;
    fec.resize(static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));
    oxr::fec::Encode(payloads, payloadSizes, 2, reinterpret_cast<uint8_t*>(fec.data()));

    assembler.addPacket(videoHeader(8,
                                    0,
                                    2,
                                    static_cast<uint16_t>(first.size())),
                        first.constData(),
                        first.size(),
                        10);
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(8,
                                        0,
                                        2,
                                        static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD),
                                        oxr::protocol::VIDEO_FLAG_FEC |
                                            oxr::protocol::VIDEO_FLAG_STEREO,
                                        static_cast<uint16_t>(missing.size())),
                            fec.constData(),
                            fec.size(),
                            11);

    expect(frames.size() == 1, "Expected FEC to recover final short packet");
    expect(frames.first().nalUnit.size() == first.size() + missing.size(),
           "Expected recovered frame to use final packet size metadata");
    expect(frames.first().nalUnit.mid(first.size()) == missing,
           "Expected recovered final packet bytes without padding");
}

void testTrackingFlagsAndMovementTargets()
{
    using namespace oxrsys::qt_simulator;
    SimulatorTrackingPose pose;

    QSet<int> keys;
    keys.insert(Qt::Key_W);
    advanceSimulatorTracking(pose, {}, keys, 1.0f);
    expect(pose.headPosition[2] < -1.9f, "Expected unmodified movement to move head");
    expect(pose.leftHandOffset[2] == -0.35f, "Expected head movement to leave the left hand's offset alone");

    SimulatorTrackingPose shiftedPose;
    QSet<int> shiftedKeys;
    shiftedKeys.insert(Qt::Key_W);
    shiftedKeys.insert(LeftShiftKey);
    advanceSimulatorTracking(shiftedPose, {}, shiftedKeys, 1.0f);
    expect(shiftedPose.headPosition[2] == 0.0f,
           "Expected left-shift movement to leave head in place");
    expect(shiftedPose.leftHandOffset[2] < -1.9f, "Expected left-shift movement to move the left hand");

    SimulatorTrackingPose rightShiftedPose;
    QSet<int> rightShiftedKeys;
    rightShiftedKeys.insert(Qt::Key_W);
    rightShiftedKeys.insert(RightShiftKey);
    advanceSimulatorTracking(rightShiftedPose, {}, rightShiftedKeys, 1.0f);
    expect(rightShiftedPose.headPosition[2] == 0.0f,
           "Expected right-shift movement to leave head in place");
    expect(rightShiftedPose.rightHandOffset[2] < -1.9f, "Expected right-shift movement to move the right hand");

    oxr::protocol::TrackingPacket packet = {};
    fillSimulatorTrackingPacket(shiftedPose, shiftedKeys, 42, 100.0f, 1.0f, packet);
    expect((packet.trackingFlags & oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0,
           "Expected left controller active flag");
    expect((packet.trackingFlags & oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0,
           "Expected right controller active flag");
    expect(packet.timestampNs == 42, "Expected caller-provided monotonic timestamp");
    expect(packet.eyeFov[0] < 0.0f && packet.eyeFov[1] > 0.0f &&
               packet.eyeFov[2] > 0.0f && packet.eyeFov[3] < 0.0f,
           "Expected simulator tracking packet to include eye FOV");
}

void testMouseLookVerticalNotInverted()
{
    using namespace oxrsys::qt_simulator;
    SimulatorTrackingPose pose;
    advanceSimulatorTracking(pose, QPointF(0.0, 100.0), {}, 0.0f);
    expect(pose.pitch < 0.0f, "Expected moving the mouse down to look down");
}

void testTouchControllerKeys()
{
    using namespace oxrsys::qt_simulator;
    using namespace oxr::protocol;
    SimulatorTrackingPose pose;
    TrackingPacket idle = {};
    fillSimulatorTrackingPacket(pose, {}, 0, 100.0f, 1.0f, idle);
    expect(idle.buttonState == 0 && idle.leftTrigger == 0.0f && idle.rightThumbstick[0] == 0.0f,
           "Expected no input with no keys held");

    const QSet<int> keys = {Qt::Key_T, Qt::Key_H, Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4,
                            Qt::Key_M, Qt::Key_C, Qt::Key_N, Qt::Key_I, Qt::Key_L, Qt::Key_Left,
                            Qt::Key_Down};
    TrackingPacket packet = {};
    fillSimulatorTrackingPacket(pose, keys, 0, 100.0f, 1.0f, packet);
    const uint32_t expected = BUTTON_LEFT_TRIGGER | BUTTON_RIGHT_TRIGGER | BUTTON_X | BUTTON_Y | BUTTON_A |
                              BUTTON_B | BUTTON_MENU | BUTTON_LEFT_THUMBSTICK | BUTTON_RIGHT_THUMBSTICK;
    expect(packet.buttonState == expected, "Expected every Touch button key to set its bit");
    expect(packet.leftTrigger == 1.0f && packet.rightTrigger == 1.0f, "Expected T and H to pull the triggers");
    expect(packet.leftThumbstick[0] == 1.0f && packet.leftThumbstick[1] == 1.0f,
           "Expected I and L to push the left thumbstick up and right");
    expect(packet.rightThumbstick[0] == -1.0f && packet.rightThumbstick[1] == -1.0f,
           "Expected Left and Down to push the right thumbstick left and down");
}

// A hand's laser must meet the gaze point however the head is turned; this failed with hands pinned in the world.
void testHandsAimAtGaze()
{
    using namespace oxrsys::qt_simulator;
    using namespace oxr::protocol;
    SimulatorTrackingPose pose;
    pose.yaw = 1.2f;
    pose.pitch = -0.4f;
    pose.headPosition[0] = 3.0f;
    TrackingPacket packet = {};
    fillSimulatorTrackingPacket(pose, {}, 0, 100.0f, 1.0f, packet);
    const float* p = packet.rightControllerPos;
    const float* q = packet.rightControllerRot;
    // Forward (0,0,-1) rotated by the hand's quaternion, extended to the gaze point's distance.
    const float fx = -2.0f * (q[0] * q[2] + q[3] * q[1]);
    const float fy = -2.0f * (q[1] * q[2] - q[3] * q[0]);
    const float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    const float h = std::sqrt((p[0] - 3.0f) * (p[0] - 3.0f) + (p[1] - 1.6f) * (p[1] - 1.6f) + p[2] * p[2]);
    expect(h < 0.6f, "Expected the hand within reach of the head after walking");
    const float gx = 3.0f - 2.0f * std::sin(1.2f) * std::cos(-0.4f);
    const float gy = 1.6f + 2.0f * std::sin(-0.4f);
    const float gz = -2.0f * std::cos(1.2f) * std::cos(-0.4f);
    const float dx = gx - p[0], dy = gy - p[1], dz = gz - p[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float miss = std::sqrt((p[0] + fx * d - gx) * (p[0] + fx * d - gx) + (p[1] + fy * d - gy) * (p[1] + fy * d - gy) +
                                 (p[2] + fz * d - gz) * (p[2] + fz * d - gz));
    expect(miss < 0.01f, "Expected the hand's laser to pass within 1 cm of the gaze point");
}

void testControllersPresentFlag()
{
    using namespace oxrsys::qt_simulator;
    using namespace oxr::protocol;
    const uint32_t both = TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE | TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    SimulatorTrackingPose pose;
    TrackingPacket present = {};
    fillSimulatorTrackingPacket(pose, {Qt::Key_H}, 0, 100.0f, 1.0f, present);
    expect((present.trackingFlags & both) == both, "Expected both controllers active by default");
    TrackingPacket absent = {};
    fillSimulatorTrackingPacket(pose, {Qt::Key_H}, 0, 100.0f, 1.0f, absent, false);
    expect((absent.trackingFlags & both) == 0, "Expected no controller active with controllers off");
    expect(absent.rightTrigger == 1.0f, "Expected input state kept with controllers off");
}

} // namespace

int main()
{
    try
    {
        testCompleteVideoFrame();
        testDuplicatePacketIgnored();
        testIncompleteFrameDrop();
        testRenderPoseIgnored();
        testFecRecovery();
        testFecRecoveryUsesFinalShortPacketSize();
        testTrailingFecAfterCompleteFrameIgnored();
        testTrackingFlagsAndMovementTargets();
        testMouseLookVerticalNotInverted();
        testTouchControllerKeys();
        testControllersPresentFlag();
        testHandsAimAtGaze();
    }
    catch (const std::exception& error)
    {
        qWarning("Simulator shared tests failed: %s", error.what());
        return 1;
    }
    return 0;
}
