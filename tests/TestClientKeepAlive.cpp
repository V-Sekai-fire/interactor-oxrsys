// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "ClientKeepAlive.h"
#include "TrackingReceiver.h"

#include <oxrsys/protocol/Protocol.h>

using namespace oxrsys::client_keepalive;

namespace
{
constexpr int64_t kMs = 1'000'000;
}

TEST_CASE("A client silent for 2.1 s after its last packet is dropped", "[streaming][keepalive]")
{
    const int64_t connectedNs = 10'000 * kMs;
    const int64_t lastPacketNs = connectedNs + 500 * kMs;
    CHECK(IsSilent(connectedNs, lastPacketNs, lastPacketNs + 2100 * kMs));
    CHECK(SilenceNs(connectedNs, lastPacketNs, lastPacketNs + 2100 * kMs) == 2100 * kMs);
}

TEST_CASE("A client sending every 100 ms for 5 s is kept", "[streaming][keepalive]")
{
    const int64_t connectedNs = 10'000 * kMs;
    int64_t lastPacketNs = 0;
    int silentTicks = 0;
    for (int64_t t = connectedNs; t <= connectedNs + 5000 * kMs; t += 10 * kMs)
    {
        if ((t - connectedNs) % (100 * kMs) == 0)
        {
            lastPacketNs = t;
        }
        silentTicks += IsSilent(connectedNs, lastPacketNs, t) ? 1 : 0;
    }
    CHECK(silentTicks == 0);
}

TEST_CASE("A client that never sends is dropped 2 s after connecting", "[streaming][keepalive]")
{
    const int64_t connectedNs = 10'000 * kMs;
    CHECK_FALSE(IsSilent(connectedNs, 0, connectedNs + 1900 * kMs));
    CHECK(IsSilent(connectedNs, 0, connectedNs + 2100 * kMs));
}

TEST_CASE("Packets from before a connect do not keep the new client alive", "[streaming][keepalive]")
{
    const int64_t lastPacketNs = 10'000 * kMs;
    const int64_t connectedNs = lastPacketNs + 60'000 * kMs;
    CHECK_FALSE(IsSilent(connectedNs, lastPacketNs, connectedNs + 1000 * kMs));
    CHECK(SilenceNs(connectedNs, lastPacketNs, connectedNs + 1000 * kMs) == 1000 * kMs);
}

TEST_CASE("TrackingReceiver stamps every injected packet, reordered ones included", "[streaming][keepalive]")
{
    TrackingReceiver receiver;
    CHECK(receiver.GetLastPacketReceiveNs() == 0);

    oxr::protocol::TrackingPacket packet = {};
    packet.headOrientation[3] = 1.0f;
    packet.timestampNs = 200;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    const int64_t first = receiver.GetLastPacketReceiveNs();
    CHECK(first > 0);

    packet.timestampNs = 100;
    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    CHECK(receiver.GetReorderedDropCount() == 1);
    CHECK(receiver.GetLastPacketReceiveNs() >= first);

    receiver.InjectPacket(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet) - 1);
    CHECK(receiver.GetReorderedDropCount() == 1);
}
