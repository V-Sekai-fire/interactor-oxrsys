# SPDX-License-Identifier: MPL-2.0
#
# The OXRSys head-mounted display for the PC VR runtime's driver interface, laid out as a driver
# root: build/<...>/driver/oxrsys/{driver.vrdrivermanifest,bin/win64/driver_oxrsys.dll}.

set(OXRSYS_DRIVER_ROOT ${CMAKE_CURRENT_BINARY_DIR}/oxrsys)
find_package(Vulkan REQUIRED)
set(OXRSYS_RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime/src)

add_library(driver_oxrsys SHARED
    src/Driver.cpp
    ${OXRSYS_RUNTIME_SRC}/Config.cpp
    ${OXRSYS_RUNTIME_SRC}/ControllerLayout.cpp
    ${OXRSYS_RUNTIME_SRC}/RuntimePlatform.cpp
    ${OXRSYS_RUNTIME_SRC}/RuntimeSockets.cpp
    ${OXRSYS_RUNTIME_SRC}/RuntimeStatus.cpp
    ${OXRSYS_RUNTIME_SRC}/StreamingFrameQueue.cpp
    ${OXRSYS_RUNTIME_SRC}/StreamingServer.cpp
    ${OXRSYS_RUNTIME_SRC}/BodyOverlay.cpp
    ${OXRSYS_RUNTIME_SRC}/TrackingReceiver.cpp
    ${OXRSYS_RUNTIME_SRC}/VulkanDispatch.cpp
    ${OXRSYS_RUNTIME_SRC}/D3D11Interop.cpp
    ${OXRSYS_RUNTIME_SRC}/PyroWaveVideoEncoder.cpp
)
target_include_directories(driver_oxrsys PRIVATE
    abi
    ${OXRSYS_RUNTIME_SRC}
    ${CMAKE_SOURCE_DIR}/common/protocol/include
    ${openxr_SOURCE_DIR}/include
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(driver_oxrsys PRIVATE
    XR_USE_GRAPHICS_API_VULKAN
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    OXRSYS_VERSION_MAJOR=${OXRSYS_VERSION_MAJOR}
    OXRSYS_VERSION_MINOR=${OXRSYS_VERSION_MINOR}
    OXRSYS_VERSION_PATCH=${OXRSYS_VERSION_PATCH}
)
if(MSVC)
    target_compile_options(driver_oxrsys PRIVATE /utf-8)
endif()
target_link_libraries(driver_oxrsys PRIVATE spdlog::spdlog glm::glm oxrsys_pyrowave ws2_32 d3d11 dxgi d3dcompiler)
set_target_properties(driver_oxrsys PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${OXRSYS_DRIVER_ROOT}/bin/win64
    LIBRARY_OUTPUT_DIRECTORY ${OXRSYS_DRIVER_ROOT}/bin/win64
)
configure_file(driver.vrdrivermanifest ${OXRSYS_DRIVER_ROOT}/driver.vrdrivermanifest COPYONLY)
file(GLOB OXRSYS_DRIVER_ICONS ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.png)
foreach(icon ${OXRSYS_DRIVER_ICONS})
    get_filename_component(icon_name ${icon} NAME)
    configure_file(${icon} ${OXRSYS_DRIVER_ROOT}/resources/icons/${icon_name} COPYONLY)
endforeach()
foreach(model_dir oxrsys_controller oxrsys_hidden)
    file(GLOB OXRSYS_DRIVER_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/resources/rendermodels/${model_dir}/*)
    foreach(model ${OXRSYS_DRIVER_MODEL})
        get_filename_component(model_name ${model} NAME)
        configure_file(${model} ${OXRSYS_DRIVER_ROOT}/resources/rendermodels/${model_dir}/${model_name} COPYONLY)
    endforeach()
endforeach()
file(GLOB OXRSYS_DRIVER_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/resources/input/*.json)
foreach(profile ${OXRSYS_DRIVER_INPUT})
    get_filename_component(profile_name ${profile} NAME)
    configure_file(${profile} ${OXRSYS_DRIVER_ROOT}/resources/input/${profile_name} COPYONLY)
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "No Python 3: the driver ABI checks (vtbl self-test, freshness, slot probe "
                    "negative control) are not registered")
    return()
endif()

set(OXRSYS_VTBL ${CMAKE_CURRENT_SOURCE_DIR}/abi/openvr_driver.vtbl)
set(OXRSYS_VTBL_PY ${CMAKE_CURRENT_SOURCE_DIR}/abi/vtbl.py)

add_executable(oxrsys_driver_slot_probe tests/SlotProbe.cpp)
target_include_directories(oxrsys_driver_slot_probe PRIVATE abi)
add_test(NAME oxrsys_driver_slot_probe COMMAND oxrsys_driver_slot_probe)

set(OXRSYS_SWAPPED_DIR ${CMAKE_CURRENT_BINARY_DIR}/swapped)
add_custom_command(
    OUTPUT ${OXRSYS_SWAPPED_DIR}/openvr_driver_min.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OXRSYS_SWAPPED_DIR}
    COMMAND Python3::Interpreter ${OXRSYS_VTBL_PY} swapped ${OXRSYS_VTBL} ${OXRSYS_SWAPPED_DIR}/openvr_driver_min.h
    DEPENDS ${OXRSYS_VTBL} ${OXRSYS_VTBL_PY}
)
add_executable(oxrsys_driver_slot_probe_swapped tests/SlotProbe.cpp ${OXRSYS_SWAPPED_DIR}/openvr_driver_min.h)
target_include_directories(oxrsys_driver_slot_probe_swapped PRIVATE ${OXRSYS_SWAPPED_DIR} abi)
add_test(NAME oxrsys_driver_slot_probe_swapped COMMAND oxrsys_driver_slot_probe_swapped)
set_tests_properties(oxrsys_driver_slot_probe_swapped PROPERTIES WILL_FAIL TRUE)

add_test(NAME oxrsys_driver_vtbl_self_test COMMAND Python3::Interpreter ${OXRSYS_VTBL_PY} self-test)
add_test(NAME oxrsys_driver_vtbl_fresh
    COMMAND Python3::Interpreter ${OXRSYS_VTBL_PY} fresh ${OXRSYS_VTBL} ${CMAKE_CURRENT_SOURCE_DIR}/abi/openvr_driver_min.h)

# The reference diff reads the interface SDK's driver header as text; pass its path to run it.
set(OXRSYS_OPENVR_REFERENCE_HEADER "" CACHE FILEPATH "Interface SDK driver header for the slot reference diff")
if(OXRSYS_OPENVR_REFERENCE_HEADER)
    add_test(NAME oxrsys_driver_vtbl_refdiff
        COMMAND Python3::Interpreter ${OXRSYS_VTBL_PY} refdiff ${OXRSYS_VTBL} ${OXRSYS_OPENVR_REFERENCE_HEADER})
else()
    message(STATUS "OXRSYS_OPENVR_REFERENCE_HEADER unset: the driver slot reference diff is not registered")
endif()
