# main/CMakeLists.txt
#
# Per-rev source selection happens here. The board's board_config.cmake
# exports BOARD_IMU_DRIVER. Capability flags (BOARD_HAS_*) are parsed
# directly out of boards/${BOARD}/board.h by parse_board_capabilities()
# below — board.h is the single source of truth.

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
include(${CMAKE_SOURCE_DIR}/boards/${BOARD}/board_config.cmake)

# Parse capability flags out of board.h into CMake variables.
parse_board_capabilities(${CMAKE_SOURCE_DIR}/boards/${BOARD}/board.h)

# Sanity check: if the parse silently failed, the conditional source list
# below would happily build the wrong combination of stubs/real drivers.
if(NOT DEFINED BOARD_HAS_IMU_LOWG)
    message(FATAL_ERROR
        "parse_board_capabilities failed to extract BOARD_HAS_IMU_LOWG from "
        "boards/${BOARD}/board.h — check the #define format.")
endif()

# Always-compiled sources
set(SRCS
    main.c
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

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        .
        hal
        drivers
        ${CMAKE_SOURCE_DIR}/boards/${BOARD}
    REQUIRES
        nvs_flash
        esp_timer
        driver
)
