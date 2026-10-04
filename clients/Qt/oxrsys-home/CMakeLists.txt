# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(OXRSysHome LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    enable_testing()

    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DetectQt6.cmake")
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

    set(OXRSYS_PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../common/protocol/include")
endif()

set(OXRSYS_HOME_CORE_SOURCES
    src/AdbBridge.cpp
    src/HomeModel.cpp
    src/Launcher.cpp
    src/PlatformSupport.cpp
    src/RuntimeActivity.cpp
    src/RuntimeManager.cpp
    src/ServerConfig.cpp
)

add_executable(oxrsys-home
    ${OXRSYS_HOME_CORE_SOURCES}
    src/HomeTray.cpp
    src/MainWindow.cpp
    src/main.cpp
)

# Tray icons rendered from resources/tray.svg by resources/render_tray.ps1.
qt_add_resources(oxrsys-home "tray" PREFIX "/tray" BASE resources
    FILES resources/tray_idle.png resources/tray_streaming.png)

target_include_directories(oxrsys-home PRIVATE
    src
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
)

# A GUI app on Windows: it lives in the tray, so it opens no console window.
set_target_properties(oxrsys-home PROPERTIES WIN32_EXECUTABLE TRUE)

target_link_libraries(oxrsys-home PRIVATE
    Qt6::Widgets
    Qt6::Network
)

add_executable(oxrsys-home-tests
    ${OXRSYS_HOME_CORE_SOURCES}
    tests/HomeCoreTests.cpp
)

target_include_directories(oxrsys-home-tests PRIVATE
    src
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
)

target_link_libraries(oxrsys-home-tests PRIVATE
    Qt6::Widgets
    Qt6::Network
)

add_test(NAME oxrsys-home-tests COMMAND oxrsys-home-tests)
