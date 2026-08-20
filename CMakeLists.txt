cmake_minimum_required(VERSION 3.21)

project(TimeSync VERSION 1.0.0 LANGUAGES CXX)

option(TIMESYNC_BUILD_TESTS "Build focused backend tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 6.9.1 REQUIRED COMPONENTS Core Network Widgets)

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
    src/ui/main_window.cpp
    src/ui/server_editor_dialog.cpp
    src/ui/ui_strings.cpp
)
set(TIMESYNC_UI_HEADERS
    src/ui/main_window.h
    src/ui/server_editor_dialog.h
    src/ui/ui_strings.h
    src/ui/ui_types.h
)

add_executable(TimeSync WIN32
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
    target_sources(TimeSync PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/timesync_manifest.rc")
endif()

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
