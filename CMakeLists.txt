# OneCollar firmware — root CMakeLists.txt
#
# Multi-revision ESP-IDF project. Selects hardware revision via the BOARD
# CMake variable. Default is rev6 (currently shipping). To target Rev 7:
#
#   idf.py -DBOARD=rev7 build
#
# The BOARD value drives:
#   - Which board.h is on the include path (boards/${BOARD}/board.h)
#   - Which IMU driver is compiled (selected in main/CMakeLists.txt)
#   - Which sdkconfig.defaults overlay applies
#   - Which model directory ships with the firmware (models/${BOARD}/)

cmake_minimum_required(VERSION 3.16)

# Default to rev6 if not specified
if(NOT DEFINED BOARD)
    set(BOARD "rev6" CACHE STRING "Hardware revision (rev6 | rev7)")
endif()

# Validate
set(VALID_BOARDS "rev6;rev7")
if(NOT BOARD IN_LIST VALID_BOARDS)
    message(FATAL_ERROR
        "Invalid BOARD '${BOARD}'. Must be one of: ${VALID_BOARDS}.\n"
        "Build with: idf.py -DBOARD=rev6 build  (or -DBOARD=rev7)")
endif()

message(STATUS "==========================================")
message(STATUS " OneCollar firmware — building for ${BOARD}")
message(STATUS "==========================================")

# Apply shared defaults plus rev-specific overlay
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(onecollar)
