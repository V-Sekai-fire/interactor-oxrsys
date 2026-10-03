# SPDX-License-Identifier: MPL-2.0

add_library(oxrsys-simulator-shared STATIC
    src/SimulatorTracking.cpp
    src/SimulatorWidget.cpp
    src/VideoFrameAssembler.cpp
)

# PyroWave decodes the stream on the GPU; the Lean-authored kernels/simulator SPIR-V turns it into pixels.
target_sources(oxrsys-simulator-shared PRIVATE src/PyroWaveDecoder.cpp)
target_include_directories(oxrsys-simulator-shared PRIVATE ${CMAKE_SOURCE_DIR}/kernels/simulator/spirv)
target_link_libraries(oxrsys-simulator-shared PRIVATE oxrsys_pyrowave)

target_include_directories(oxrsys-simulator-shared PUBLIC
    src
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
)

target_link_libraries(oxrsys-simulator-shared PUBLIC
    Qt6::Widgets
    Qt6::Network
)

add_executable(oxrsys-simulator-shared-tests
    tests/SimulatorSharedTests.cpp
)

target_link_libraries(oxrsys-simulator-shared-tests PRIVATE
    oxrsys-simulator-shared
)

add_test(NAME oxrsys-simulator-shared-tests COMMAND oxrsys-simulator-shared-tests)
