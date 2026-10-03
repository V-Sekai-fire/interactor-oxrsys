// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cstdint>

namespace oxrsys::client_keepalive
{

// The client's ~90 Hz tracking packets are the keep-alive.
constexpr int64_t kSilenceTimeoutNs = 2'000'000'000;

// Measured from the later of connect and last packet, so a previous client's packets never count.
inline int64_t SilenceNs(int64_t connectedNs, int64_t lastPacketNs, int64_t nowNs)
{
    return nowNs - std::max(connectedNs, lastPacketNs);
}

inline bool IsSilent(int64_t connectedNs, int64_t lastPacketNs, int64_t nowNs,
                     int64_t timeoutNs = kSilenceTimeoutNs)
{
    return SilenceNs(connectedNs, lastPacketNs, nowNs) > timeoutNs;
}

} // namespace oxrsys::client_keepalive
