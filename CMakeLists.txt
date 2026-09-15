cmake_minimum_required(VERSION 3.22)

project(TimeSync VERSION 1.0.0 LANGUAGES CXX)

option(TIMESYNC_BUILD_TESTS "Build focused backend tests" ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 6.9.1 REQUIRED COMPONENTS Core Network Widgets)

get_target_property(TIMESYNC_QT_CORE_TYPE Qt6::Core TYPE)
set(TIMESYNC_STATIC_QT OFF)
if(TIMESYNC_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY"
   OR (DEFINED QT6_IS_SHARED_LIBS_BUILD AND NOT QT6_IS_SHARED_LIBS_BUILD))
    set(TIMESYNC_STATIC_QT ON)
endif()
message(STATUS "TimeSync Qt linkage: static=${TIMESYNC_STATIC_QT}")

set(TIMESYNC_BACKEND_SOURCES
    src/core/config_repository.cpp
    src/core/operation_types.cpp
    src/core/result.cpp
    src/core/server_endpoint.cpp
    src/core/sync_coordinator.cpp
    src/core/trusted_clock.cpp
    src/network/ntp_client.cpp
    src/network/ntp_protocol.cpp
    src/platform/cross_process_mutex.cpp
    src/platform/elevation_broker.cpp
    src/platform/power_resume_monitor.cpp
    src/platform/result_handoff.cpp
    src/platform/task_scheduler.cpp
    src/platform/windows_system_clock.cpp
    src/cli/cli_parser.cpp
    src/cli/cli_runner.cpp
)

add_library(timesync_backend STATIC ${TIMESYNC_BACKEND_SOURCES})
target_include_directories(timesync_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(timesync_backend PUBLIC Qt6::Core Qt6::Network)

if(WIN32)
    target_link_libraries(timesync_backend PUBLIC
        advapi32
        ole32
        oleaut32
        shell32
        taskschd
        user32
        uuid
        ws2_32
    )
endif()

set(TIMESYNC_UI_SOURCES
    src/ui/about_dialog.cpp
    src/ui/main_window.cpp
    src/ui/server_editor_dialog.cpp
    src/ui/ui_strings.cpp
)
set(TIMESYNC_UI_HEADERS
    src/ui/about_dialog.h
    src/ui/brand_assets.h
    src/ui/main_window.h
    src/ui/server_editor_dialog.h
    src/ui/ui_strings.h
    src/ui/ui_types.h
)

qt_add_executable(TimeSync MANUAL_FINALIZATION WIN32
    src/app/main.cpp
    src/app/app_controller.cpp
    src/app/app_controller.h
    ${TIMESYNC_UI_SOURCES}
    ${TIMESYNC_UI_HEADERS}
)
target_link_libraries(TimeSync PRIVATE timesync_backend Qt6::Widgets)
target_compile_definitions(TimeSync PRIVATE TIMESYNC_HAS_UI)

target_sources(TimeSync PRIVATE resources/timesync.qrc)

if(WIN32)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/timesync_manifest.rc"
        PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/TimeSync.manifest;${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/timesync.ico"
    )
    target_sources(TimeSync PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/timesync_manifest.rc")
endif()

if(TIMESYNC_STATIC_QT)
    if(NOT TARGET Qt6::QWindowsIntegrationPlugin)
        message(FATAL_ERROR "Static Qt kit does not provide QWindowsIntegrationPlugin")
    endif()
    if(NOT TARGET Qt6::QSchannelBackendPlugin)
        message(FATAL_ERROR "Static Qt kit does not provide QSchannelBackendPlugin; rebuild Qt with Schannel")
    endif()
    qt_import_plugins(TimeSync
        NO_DEFAULT
        INCLUDE
            Qt6::QWindowsIntegrationPlugin
            Qt6::QSchannelBackendPlugin
    )
    if(MINGW)
        target_link_options(TimeSync PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

if(MINGW)
    target_compile_options(TimeSync PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(TimeSync PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-Wl,--gc-sections;-s>
    )
endif()

qt_finalize_target(TimeSync)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Deploy.cmake")

include(CTest)
if(BUILD_TESTING AND TIMESYNC_BUILD_TESTS)
    find_package(Qt6 6.9.1 COMPONENTS Test QUIET)
    if(TARGET Qt6::Test)
        add_executable(timesync_backend_tests
            tests/backend_tests.cpp
        )
        target_link_libraries(timesync_backend_tests PRIVATE
            timesync_backend
            Qt6::Test
        )
        add_test(NAME timesync_backend_tests COMMAND timesync_backend_tests)
    else()
        message(STATUS "Qt6::Test not available; focused backend tests are disabled")
    endif()
endif()
