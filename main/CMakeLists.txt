# main/CMakeLists.txt
#
# Per-rev source selection happens here. The board's board_config.cmake
# exports BOARD_IMU_DRIVER and BOARD_HAS_* flags that we use to decide which
# .c files compile into the binary.

# Pull in the active board's config (sets BOARD_IMU_DRIVER, BOARD_HAS_*)
include(${CMAKE_SOURCE_DIR}/boards/${BOARD}/board_config.cmake)

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
if(BOARD_HAS_SFLP EQUAL 0)
    list(APPEND SRCS drivers/sflp_iface_stub.c)
endif()

# High-g: same pattern as SFLP.
if(BOARD_HAS_HIGHG EQUAL 0)
    list(APPEND SRCS drivers/highg_iface_stub.c)
endif()

# Audio: real driver on Rev 7, stub on Rev 6.
if(BOARD_HAS_AUDIO EQUAL 1)
    list(APPEND SRCS drivers/audio_i2s_mems.c)
else()
    list(APPEND SRCS drivers/audio_iface_stub.c)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        .
        hal
        ${CMAKE_SOURCE_DIR}/boards/${BOARD}
    REQUIRES
        nvs_flash
        esp_timer
        driver
)
