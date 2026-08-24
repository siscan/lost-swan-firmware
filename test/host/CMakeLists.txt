# Host unit tests - plain CMake, no ESP-IDF (CLAUDE.md toolchain).
#
#   cmake -S test/host -B build_host
#   cmake --build build_host
#   ctest --test-dir build_host --output-on-failure
#
# Only pure sources are compiled here.  Anything that includes an IDF header
# belongs behind hal/ or a *_store/*_service shell and is not host-testable by
# design.
cmake_minimum_required(VERSION 3.16)
project(swan_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The simulated-axis suite runs hundreds of millions of 20 us ticks; an
# unoptimised build makes it crawl.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

set(SWAN_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# Pure sources are compiled straight into each test executable rather than into
# a static library: the suite must stay free of `ar` (an Application Control
# policy blocks ar.exe on the Windows dev machine).
set(SWAN_PURE_SRC
    ${SWAN_ROOT}/components/timesvc/tz.cpp
    ${SWAN_ROOT}/components/frame/frame.cpp
    ${SWAN_ROOT}/components/modes/render.cpp
    ${SWAN_ROOT}/components/modes/mode_manager.cpp
    ${SWAN_ROOT}/components/modes/wear.cpp
    ${SWAN_ROOT}/components/ring/ring_runtime.cpp
    ${SWAN_ROOT}/components/ring/json_lite.cpp
    ${SWAN_ROOT}/components/webapi/api.cpp
    ${SWAN_ROOT}/components/webapi/mqtt_bridge.cpp
    ${SWAN_ROOT}/components/webapi/ha_discovery.cpp
    ${SWAN_ROOT}/components/webapi/ota_policy.cpp
    ${SWAN_ROOT}/components/webapi/portal.cpp
    ${SWAN_ROOT}/components/audio/wav.cpp
    ${SWAN_ROOT}/components/webapi/ring_upload.cpp
    ${SWAN_ROOT}/components/motion/axis_control.cpp
    ${SWAN_ROOT}/components/motion/fault_policy.cpp)

add_library(swan_test_cfg INTERFACE)
target_include_directories(swan_test_cfg INTERFACE
    ${SWAN_ROOT}/components/timesvc/include
    ${SWAN_ROOT}/components/frame/include
    ${SWAN_ROOT}/components/modes/include
    ${SWAN_ROOT}/components/ring/include
    ${SWAN_ROOT}/components/motion/include
    ${SWAN_ROOT}/components/webapi/include
    ${SWAN_ROOT}/components/audio/include
    ${SWAN_ROOT}/components/swan_hal/include
    ${CMAKE_CURRENT_LIST_DIR})

if(MSVC)
    target_compile_options(swan_test_cfg INTERFACE /W4)
else()
    target_compile_options(swan_test_cfg INTERFACE -Wall -Wextra -Wpedantic)
endif()

# test name -> optional data-file argument (argv[1], g_argv1 in check.h)
set(SWAN_TESTS test_ring test_motion_math test_axis_sim test_ring_json test_tz test_frame test_modes test_wear test_fault_policy test_mqtt_bridge test_ha_discovery test_ota test_portal test_audio test_api)
set(test_ring_json_ARGS ${SWAN_ROOT}/data/ring.json)
# Sniffed against the REAL built binary when there is one; the suite says so
# and skips that check otherwise, rather than passing on a synthetic header.
set(test_ota_ARGS ${SWAN_ROOT}/build/lost_swan_firmware.bin)

find_package(Threads REQUIRED)

foreach(t ${SWAN_TESTS})
    add_executable(${t} ${t}.cpp ${SWAN_PURE_SRC})
    target_link_libraries(${t} PRIVATE swan_test_cfg Threads::Threads)
    add_test(NAME ${t} COMMAND ${t} ${${t}_ARGS})
endforeach()

# Not a test: the trace generator for the browser simulator (web/sim).
# Run manually after mode/frame changes:
#   build_host/gen_traces data/ring.json web/sim/traces.js
add_executable(gen_traces gen_traces.cpp ${SWAN_PURE_SRC})
target_link_libraries(gen_traces PRIVATE swan_test_cfg Threads::Threads)

# Not a test: the host dev server (spec 15 phase 3, step a).  Serves web/ and
# speaks the real /ws against the real ModeManager over simulated axes, so the
# UI is clickable with no hardware:
#   build_host/devserver --port 8080 --root web --ring data/ring.json
add_executable(devserver
    ${SWAN_ROOT}/tools/devserver/devserver.cpp
    ${SWAN_ROOT}/tools/devserver/httpd.cpp
    ${SWAN_PURE_SRC})
target_include_directories(devserver PRIVATE ${SWAN_ROOT}/tools/devserver)
target_link_libraries(devserver PRIVATE swan_test_cfg Threads::Threads)
if(WIN32)
    target_link_libraries(devserver PRIVATE ws2_32)
endif()
