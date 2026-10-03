// SPDX-License-Identifier: MPL-2.0
//
// Calls every method of each generated interface on an object whose vtable entries record their
// own index, and fails when the compiled slot differs from the slot openvr_driver.vtbl declares.

#include "openvr_driver_min.h"

#include <array>
#include <cstdio>
#include <utility>

namespace
{

uint32_t gLastSlot = 0;

// The second argument is the hidden return pointer for methods returning a struct by value.
template <uint32_t N>
void* Thunk(void* /*self*/, void* returned)
{
    gLastSlot = N;
    return returned;
}

template <uint32_t... N>
std::array<void*, sizeof...(N)> MakeTable(std::integer_sequence<uint32_t, N...>)
{
    return {reinterpret_cast<void*>(&Thunk<N>)...};
}

} // namespace

int main()
{
    static std::array<void*, 64> table = MakeTable(std::make_integer_sequence<uint32_t, 64>());
    struct
    {
        void** vtable;
    } object = {table.data()};

    int failures = 0;
    int probes = 0;
    for (const oxrvr::SlotProbe& probe : oxrvr::kSlotProbes)
    {
        gLastSlot = 0xffffffffu;
        probe.call(&object);
        ++probes;
        if (gLastSlot != probe.slot)
        {
            std::printf("FAIL %s::%s declared slot %u, compiled slot %u\n", probe.interfaceName, probe.methodName,
                        probe.slot, gLastSlot);
            ++failures;
        }
    }
    std::printf("%d slots probed, %d mismatched\n", probes, failures);
    return failures == 0 && probes > 0 ? 0 : 1;
}
