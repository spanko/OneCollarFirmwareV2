# boards/rev6/board_config.cmake
#
# Exports board-specific CMake variables consumed by main/CMakeLists.txt.
# These select which driver implementations get compiled into the binary.

set(BOARD_IMU_DRIVER          "imu_lsm6dso32x.c"  CACHE INTERNAL "")
set(BOARD_TIER0_TOOLCHAIN     "unico_gui"         CACHE INTERNAL "")
set(BOARD_HAS_AUDIO           0                   CACHE INTERNAL "")
set(BOARD_HAS_SFLP            0                   CACHE INTERNAL "")
set(BOARD_HAS_HIGHG           0                   CACHE INTERNAL "")
set(BOARD_HAS_VAD             0                   CACHE INTERNAL "")
set(BOARD_HAS_SUBGHZ_RADIO    1                   CACHE INTERNAL "")

# Pre-processor define passed to the compiler so C code can #ifdef on board ID.
add_compile_definitions(ONECOLLAR_BOARD_REV6=1)
