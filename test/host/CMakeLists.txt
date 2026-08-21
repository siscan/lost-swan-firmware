# Host unit tests - plain CMake, no ESP-IDF (CLAUDE.md toolchain).
#
#   cmake -S test/host -B build/host
#   cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
#
# Only pure sources are compiled here.  Anything that includes an IDF header
# belongs behind hal/ and is not host-testable by design.
cmake_minimum_required(VERSION 3.16)
project(swan_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

set(SWAN_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# Pure sources are compiled straight into each test executable rather than into
# a static library.  Two binaries sharing one translation unit gain nothing from
# an archive, and skipping it means the suite never invokes `ar` - which some
# locked-down Windows machines block via Application Control policy.
set(SWAN_PURE_SRC ${SWAN_ROOT}/components/ring/ring.cpp)

add_library(swan_test_cfg INTERFACE)
target_include_directories(swan_test_cfg INTERFACE
    ${SWAN_ROOT}/components/ring/include
    ${SWAN_ROOT}/components/motion/include
    ${CMAKE_CURRENT_LIST_DIR})

if(MSVC)
    target_compile_options(swan_test_cfg INTERFACE /W4)
else()
    target_compile_options(swan_test_cfg INTERFACE -Wall -Wextra -Wpedantic)
endif()

foreach(t test_ring test_motion_math)
    add_executable(${t} ${t}.cpp ${SWAN_PURE_SRC})
    target_link_libraries(${t} PRIVATE swan_test_cfg)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
