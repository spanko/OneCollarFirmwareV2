# boards/rev7/board_config.cmake
#
# Exports board-specific CMake variables consumed by main/CMakeLists.txt.
#
# Note: BOARD_HAS_* capability flags are NOT declared here. They live in
# boards/rev7/board.h (the single source of truth) and are parsed into CMake
# variables at configure time by parse_board_capabilities() in main/CMakeLists.txt.

set(BOARD_IMU_DRIVER          "imu_lsm6dsv320x.c" CACHE INTERNAL "")
set(BOARD_TIER0_TOOLCHAIN     "mems_studio"       CACHE INTERNAL "")

# Skip in script mode -- this file is also include()'d by ESP-IDF's component
# requirements scanner (cmake -P), where add_compile_definitions is unavailable.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_compile_definitions(ONECOLLAR_BOARD_REV7=1)
endif()
