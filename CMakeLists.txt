# LOST Swan split-flap — firmware
# ESP-IDF 5.5.x, target esp32c5.  See README.md for build instructions.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Board select.  The pin map is one header (hal/pins.h) chosen by this define,
# so XIAO vs DevKitC-1 is a build flag and not a code change (CLAUDE.md).
#
#   idf.py build                     -> DevKitC-1 (spec 2.2)
#   idf.py -DSWAN_BOARD=xiao build   -> XIAO      (spec 2.1)
set(SWAN_BOARD "devkitc1" CACHE STRING "Target board: devkitc1 | xiao")

if(SWAN_BOARD STREQUAL "xiao")
    idf_build_set_property(COMPILE_DEFINITIONS "BOARD_XIAO_ESP32C5=1" APPEND)
elseif(SWAN_BOARD STREQUAL "devkitc1")
    idf_build_set_property(COMPILE_DEFINITIONS "BOARD_ESP32C5_DEVKITC1=1" APPEND)
else()
    message(FATAL_ERROR "SWAN_BOARD must be 'devkitc1' or 'xiao', got '${SWAN_BOARD}'")
endif()
message(STATUS "LOST Swan board: ${SWAN_BOARD}")

# --- Simulated axes (spec 5.9) ----------------------------------------------
# Compiled IN by default, because the whole point is that it is a RUNTIME
# choice: a fresh NVS still boots every column real (column_mode.h static-
# asserts that), and nothing simulates itself by accident.
#
# SWAN_RELEASE=1 is the gate for a build that must not be able to simulate at
# all.  It is an error to ask for both: a release image with the sim compiled
# in is exactly the thing that could be mistaken for the real display.
option(SWAN_SIM_AXES "Compile the simulated-drum axes in (runtime-selectable)" ON)
option(SWAN_RELEASE "Release build: refuse anything that could fake the mechanism" OFF)
# The stand-in bench build (BRINGUP 28b gate 3).  One real column on a PRINTED
# PLA axle: the speed cap is compiled in and there is no key that lifts it.
option(SWAN_BENCH "Stand-in bench build: hard speed cap, no show spin" OFF)

if(SWAN_RELEASE AND SWAN_SIM_AXES)
    message(FATAL_ERROR
        "SWAN_RELEASE=1 with SWAN_SIM_AXES=ON: a release image must not contain "
        "simulated axes. Build with -DSWAN_SIM_AXES=OFF.")
endif()

# A bench image is a BENCH image.  It caps every commanded speed at one drum
# revolution per second because the axle is printed, so it must never be
# mistaken for something that can drive the finished machine.
if(SWAN_BENCH AND SWAN_RELEASE)
    message(FATAL_ERROR
        "SWAN_BENCH=1 with SWAN_RELEASE=1: a capped bench image is not a "
        "release image. Pick one.")
endif()

idf_build_set_property(COMPILE_DEFINITIONS
                       "SWAN_SIM_AXES=$<BOOL:${SWAN_SIM_AXES}>" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS
                       "SWAN_BENCH=$<BOOL:${SWAN_BENCH}>" APPEND)
if(SWAN_RELEASE)
    idf_build_set_property(COMPILE_DEFINITIONS "SWAN_RELEASE=1" APPEND)
endif()
message(STATUS "LOST Swan simulated axes: ${SWAN_SIM_AXES}  release: ${SWAN_RELEASE}")

# Build identity, carried in esp_app_desc_t and reported as sys.version.
#
# esp_app_desc has project_name / version / idf_ver / chip_id and NOTHING that
# distinguishes the XIAO pin map from the DevKitC-1 one, or a build with the
# simulated axes compiled in from a release build without them.  An OTA cannot
# refuse a wrong-board image it cannot identify, and a rollback test cannot
# prove an image was swapped if both report the same string.  So the flavour
# goes in the version:
#
#     0.4.0+devkitc1.sim      0.4.0+xiao.sim      0.4.0+devkitc1.rel
#
# Set before project(), which is where IDF reads it.
set(SWAN_VERSION "0.4.0")
if(SWAN_BENCH)
    # First in the chain deliberately: a capped image must announce itself
    # as one wherever a version string is read - the boot log, sys.version,
    # and an OTA that would otherwise put a capped image on the wall.
    set(SWAN_FLAVOUR "bench")
elseif(SWAN_RELEASE)
    set(SWAN_FLAVOUR "rel")
elseif(SWAN_SIM_AXES)
    set(SWAN_FLAVOUR "sim")
else()
    set(SWAN_FLAVOUR "nosim")
endif()
set(PROJECT_VER "${SWAN_VERSION}+${SWAN_BOARD}.${SWAN_FLAVOUR}")
message(STATUS "LOST Swan version: ${PROJECT_VER}")

project(lost_swan_firmware)

# --- LittleFS payload (spec 10.2; spec 15 phase 3, step e) -------------------
# tools/webpack.py gzips web/ and drops the generated ring.json alongside it in
# a staging directory; the image is built from that and written by `idf.py
# flash`.  Only the .gz copies ship - httpd.cpp serves them with
# Content-Encoding: gzip - and the packer fails the build if the payload
# outgrows its budget.  Whole UI + ring.json is ~14 KB against a 2048 KB
# partition, so the room is for audio (spec 9).
set(SWAN_WEBFS "${CMAKE_BINARY_DIR}/webfs")
add_custom_target(swan_webfs ALL
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/tools/webpack.py" --out "${SWAN_WEBFS}"
    BYPRODUCTS "${SWAN_WEBFS}"
    COMMENT "Staging the LittleFS payload (gzipped web assets + ring.json)"
    VERBATIM)
littlefs_create_partition_image(storage "${SWAN_WEBFS}" FLASH_IN_PROJECT DEPENDS swan_webfs)
