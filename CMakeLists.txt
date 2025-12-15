cmake_minimum_required(VERSION 3.16)
project(VPNGateManager)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)

set(SOURCES
    main.cpp
    mainwindow.cpp
    serverdownloader.cpp
    servertester.cpp
    vpnmanager.cpp
)

set(HEADERS
    mainwindow.h
    vpntypes.h
    serverdownloader.h
    servertester.h
    vpnmanager.h
)

set(FORMS
    mainwindow.ui
)

add_executable(VPNGateManager ${SOURCES} ${HEADERS} ${FORMS})

target_link_libraries(VPNGateManager
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(VPNGateManager PRIVATE QT_DEBUG)
else()
    target_compile_definitions(VPNGateManager PRIVATE QT_NO_DEBUG)
endif()

install(TARGETS VPNGateManager DESTINATION bin)
