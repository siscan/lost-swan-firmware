# Host unit tests - plain CMake, no ESP-IDF (CLAUDE.md toolchain).
#
#   cmake -S test/host -B build/host
#   cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
#
# Only pure sources are compiled here.  Anything that includes an IDF header
# belongs behind hal/ and is not host-testable by design.  The motion control
# core (axis_control.cpp) is pure, which is what makes the simulated-axis
# suite possible.
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
# a static library.  Two reasons, both load-bearing: the suite must stay free
# of `ar` (an Application Control policy blocks ar.exe on the Windows dev
# machine), and per-test compilation keeps the executables independent.
set(SWAN_PURE_SRC
    ${SWAN_ROOT}/components/ring/ring.cpp
    ${SWAN_ROOT}/components/motion/axis_control.cpp)

add_library(swan_test_cfg INTERFACE)
target_include_directories(swan_test_cfg INTERFACE
    ${SWAN_ROOT}/components/ring/include
    ${SWAN_ROOT}/components/motion/include
    ${SWAN_ROOT}/components/swan_hal/include
    ${CMAKE_CURRENT_LIST_DIR})

if(MSVC)
    target_compile_options(swan_test_cfg INTERFACE /W4)
else()
    target_compile_options(swan_test_cfg INTERFACE -Wall -Wextra -Wpedantic)
endif()

foreach(t test_ring test_motion_math test_axis_sim)
    add_executable(${t} ${t}.cpp ${SWAN_PURE_SRC})
    target_link_libraries(${t} PRIVATE swan_test_cfg)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
