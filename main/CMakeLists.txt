# main/CMakeLists.txt
#
# Per-rev source selection happens here. The board's board_config.cmake
# exports BOARD_IMU_DRIVER. Capability flags (BOARD_HAS_*) are parsed
# directly out of boards/${BOARD}/board.h by parse_board_capabilities()
# below — board.h is the single source of truth.

# ---------------------------------------------------------------------------
# Cross sub-process boundary
#
# ESP-IDF runs component_get_requirements.cmake in a fresh cmake invocation
# to scan component dependencies. That sub-process does NOT inherit the
# parent's CMakeCache.txt, so a top-level `set(BOARD ... CACHE ...)` is
# invisible here. Two consequences this block fixes:
#
#   1. ${BOARD} is empty in the requirements-scan context. The top-level
#      CMakeLists.txt also exports the value via ENV{ONECOLLAR_BOARD};
#      pull it back into a normal CMake variable.
#   2. ${CMAKE_SOURCE_DIR} points at the build directory in the sub-process,
#      not at the project root. Anchor project-relative paths to this file's
#      directory (main/) instead.
# ---------------------------------------------------------------------------
if(NOT DEFINED BOARD)
    if(DEFINED ENV{ONECOLLAR_BOARD})
        set(BOARD "$ENV{ONECOLLAR_BOARD}")
    else()
        message(FATAL_ERROR
            "BOARD not set. Build with: idf.py -DBOARD=rev6 build")
    endif()
endif()
if(NOT DEFINED BRINGUP)
    set(BRINGUP "$ENV{ONECOLLAR_BRINGUP}")
endif()
set(_OC_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# ---------------------------------------------------------------------------
# parse_board_capabilities(<board_header>)
#
# Reads <board_header>, strips C++ // line comments, and extracts every
#   #define BOARD_HAS_<NAME> <0|1>
# into a CMake variable of the same name (set in PARENT_SCOPE).
#
# This keeps board.h as the only place capability flags are declared.
# ---------------------------------------------------------------------------
function(parse_board_capabilities board_header)
    file(READ "${board_header}" header_contents)
    # Strip C++ line comments so trailing-comment values don't confuse the regex.
    string(REGEX REPLACE "//[^\n]*" "" header_contents "${header_contents}")
    string(REGEX MATCHALL "#define[ \t]+BOARD_HAS_[A-Z0-9_]+[ \t]+[01]"
           defines "${header_contents}")
    foreach(def IN LISTS defines)
        if(def MATCHES "#define[ \t]+(BOARD_HAS_[A-Z0-9_]+)[ \t]+([01])")
            set(${CMAKE_MATCH_1} ${CMAKE_MATCH_2} PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

# Pull in the active board's config (sets BOARD_IMU_DRIVER, BOARD_TIER0_TOOLCHAIN).
include(${_OC_PROJECT_ROOT}/boards/${BOARD}/board_config.cmake)

# Parse capability flags out of board.h into CMake variables.
parse_board_capabilities(${_OC_PROJECT_ROOT}/boards/${BOARD}/board.h)

# Sanity check: if the parse silently failed, the conditional source list
# below would happily build the wrong combination of stubs/real drivers.
if(NOT DEFINED BOARD_HAS_IMU_LOWG)
    message(FATAL_ERROR
        "parse_board_capabilities failed to extract BOARD_HAS_IMU_LOWG from "
        "boards/${BOARD}/board.h — check the #define format.")
endif()

# Entry point — production main.c by default; bring-up sketch when BRINGUP=ON.
if(BRINGUP)
    set(_OC_ENTRY_SRC "bringup_${BOARD}.c")
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_OC_ENTRY_SRC}")
        message(FATAL_ERROR
            "BRINGUP=ON but main/${_OC_ENTRY_SRC} does not exist for BOARD=${BOARD}.")
    endif()
    message(STATUS "Entry point: ${_OC_ENTRY_SRC} (bring-up sketch)")
else()
    set(_OC_ENTRY_SRC "main.c")
endif()

# Always-compiled sources
set(SRCS
    ${_OC_ENTRY_SRC}
    i2c_bus.c
    data_logger.c
    drivers/${BOARD_IMU_DRIVER}
    drivers/fuel_max17048.c
    drivers/ble_service.c
    drivers/lora_rfm95w.c
    drivers/gps_neo_m8q.c
    drivers/power.c
)

# SFLP: the LSM6DSV320X driver implements it; on Rev 6 we link a stub.
if(NOT BOARD_HAS_IMU_SFLP)
    list(APPEND SRCS drivers/sflp_iface_stub.c)
endif()

# High-g: same pattern as SFLP.
if(NOT BOARD_HAS_IMU_HIGHG)
    list(APPEND SRCS drivers/highg_iface_stub.c)
endif()

# Audio: real driver on Rev 7, stub on Rev 6.
if(BOARD_HAS_AUDIO)
    list(APPEND SRCS drivers/audio_i2s_mems.c)
else()
    list(APPEND SRCS drivers/audio_iface_stub.c)
endif()

# REQUIRES list. The bring-up sketch calls NimBLE and the new-style peripheral
# drivers directly (advertise + accept, i2c_master, spi_master, gpio, uart).
# The production main.c reaches those through drivers/*.c wrappers that may
# hide the deps, so we only require these components when BRINGUP is on.
# ESP-IDF v6 split the old monolithic `driver` into per-peripheral
# `esp_driver_*` components -- each direct header include needs its own.
set(REQUIRES_LIST nvs_flash esp_timer driver)
if(BRINGUP)
    list(APPEND REQUIRES_LIST
        bt
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_driver_usb_serial_jtag
        nanopb
    )
    # Generated BLE protocol bindings (nanopb). Source: ../onecollar-platform/
    # contracts/ble_protocol.proto + .options, regenerated via
    # ../onecollar-platform/tools/codegen/gen_nanopb.sh. Used by the
    # library-eval harness in bringup_rev6.c (gate 3 TimeSync).
    list(APPEND SRCS ble/ble_protocol.pb.c)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        .
        ble
        hal
        drivers
        ${_OC_PROJECT_ROOT}/boards/${BOARD}
    REQUIRES ${REQUIRES_LIST}
)
