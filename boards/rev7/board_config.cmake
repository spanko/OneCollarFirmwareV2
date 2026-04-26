# boards/rev7/board_config.cmake
#
# Exports board-specific CMake variables consumed by main/CMakeLists.txt.

set(BOARD_IMU_DRIVER          "imu_lsm6dsv320x.c" CACHE INTERNAL "")
set(BOARD_TIER0_TOOLCHAIN     "mems_studio"       CACHE INTERNAL "")
set(BOARD_HAS_AUDIO           1                   CACHE INTERNAL "")
set(BOARD_HAS_SFLP            1                   CACHE INTERNAL "")
set(BOARD_HAS_HIGHG           1                   CACHE INTERNAL "")
set(BOARD_HAS_VAD             0                   CACHE INTERNAL "")
set(BOARD_HAS_SUBGHZ_RADIO    0                   CACHE INTERNAL "")

add_compile_definitions(ONECOLLAR_BOARD_REV7=1)
