cmake_minimum_required(VERSION 3.24)

project(nux_effects_firmware LANGUAGES C ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(
    NCR2_BUILD_MCUX_USB_ADAPTER
    "Compile-check the RT1051 open recovery USB adapter"
    OFF
)
option(
    NCR2_BUILD_MCUX_FLEXSPI_ADAPTER
    "Compile-check the RT1051 RAM-resident FlexSPI NOR adapter"
    OFF
)
option(
    NCR2_BUILD_MCUX_BOARD_ADAPTER
    "Compile-check the RT1051 GPIO/USB/reset board adapter"
    OFF
)
set(
    NCR2_MCUX_SDK_ROOT
    ""
    CACHE PATH
    "Pinned workspace produced by tools/fetch_mcux_sdk.py"
)

set(NCR2_CPU_FLAGS
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
)

function(ncr2_firmware_target target linker_script)
    target_compile_options(
        ${target}
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -fno-stack-protector
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
            $<$<COMPILE_LANGUAGE:C>:-Os>
    )
    target_link_options(
        ${target}
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -nostdlib
            -Wl,--gc-sections
            -Wl,--build-id=none
            -Wl,--orphan-handling=warn
            -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map
            -T${linker_script}
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf")

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${target}>
            ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin
        BYPRODUCTS
            ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin
        VERBATIM
    )
endfunction()

add_executable(
    ncr2_bootloader
    bootloader/src/startup.S
    bootloader/src/boot_controller.c
    bootloader/src/boot_journal.c
    bootloader/src/boot_lifecycle.c
    bootloader/src/boot_recovery_request.c
    bootloader/src/bootloader.c
    bootloader/src/boot_state.c
    bootloader/src/crc32.c
    bootloader/src/recovery_engine.c
    bootloader/src/recovery_protocol.c
    bootloader/src/recovery_storage.c
    bootloader/src/sha256.c
    platform/ncr2/src/ncr2_nor.c
)
target_include_directories(
    ncr2_bootloader
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
)
ncr2_firmware_target(
    ncr2_bootloader
    ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/ncr2_bootloader.ld
)
# The ITCM load image has a low VMA and a flash LMA. GNU ld's default
# page-aligned ELF headers cannot represent that split in this small image.
target_link_options(ncr2_bootloader PRIVATE -Wl,-n)

add_executable(
    ncr2_app
    app/src/startup.S
    app/src/main.c
    bootloader/src/boot_recovery_request.c
    platform/ncr2/src/ncr2_boot_request.c
)
target_include_directories(
    ncr2_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
)
ncr2_firmware_target(
    ncr2_app
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ncr2_app.ld
)

if(NCR2_BUILD_MCUX_USB_ADAPTER)
    if(NOT EXISTS
       "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/MIMXRT1051.h")
        message(
            FATAL_ERROR
            "NCR2_MCUX_SDK_ROOT is not a pinned MCUX SDK workspace"
        )
    endif()

    set(NCR2_USB_MIDDLEWARE "${NCR2_MCUX_SDK_ROOT}/middleware/usb")
    set(NCR2_USB_OUTPUT "${NCR2_USB_MIDDLEWARE}/output/source/device")
    set(
        NCR2_USB_VENDOR_SOURCES
        ${NCR2_USB_MIDDLEWARE}/device/usb_device_ehci.c
        ${NCR2_USB_MIDDLEWARE}/device/usb_device_dci.c
        ${NCR2_USB_MIDDLEWARE}/phy/usb_phy.c
        ${NCR2_USB_OUTPUT}/usb_device_ch9.c
        ${NCR2_USB_OUTPUT}/class/usb_device_class.c
        ${NCR2_USB_OUTPUT}/class/usb_device_hid.c
    )
    add_library(
        ncr2_mcux_usb_adapter
        OBJECT
        platform/ncr2/usb/recovery_usb.c
        platform/ncr2/usb/recovery_usb_descriptor.c
        ${NCR2_USB_VENDOR_SOURCES}
    )
    target_include_directories(
        ncr2_mcux_usb_adapter
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/usb
    )
    target_include_directories(
        ncr2_mcux_usb_adapter
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
            ${NCR2_MCUX_SDK_ROOT}/core/components/lists
            ${NCR2_MCUX_SDK_ROOT}/core/components/osa
            ${NCR2_USB_MIDDLEWARE}/include
            ${NCR2_USB_MIDDLEWARE}/device
            ${NCR2_USB_MIDDLEWARE}/phy
            ${NCR2_USB_OUTPUT}
            ${NCR2_USB_OUTPUT}/class
    )
    target_compile_definitions(
        ncr2_mcux_usb_adapter
        PRIVATE
            CPU_MIMXRT1051DVL6B
            USB_STACK_BM
            FSL_OSA_BM_TASK_ENABLE=0
            FSL_OSA_BM_TIMER_CONFIG=0
            SDK_OS_BAREMETAL
            NCR2_OPEN_USB_VID=0
            NCR2_OPEN_USB_PID=0
    )
    target_compile_options(
        ncr2_mcux_usb_adapter
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
    )
    set_source_files_properties(
        ${NCR2_USB_VENDOR_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w"
    )
endif()

if(NCR2_BUILD_MCUX_FLEXSPI_ADAPTER)
    if(NOT EXISTS
       "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/MIMXRT1051.h")
        message(
            FATAL_ERROR
            "NCR2_MCUX_SDK_ROOT is not a pinned MCUX SDK workspace"
        )
    endif()

    set(
        NCR2_FLEXSPI_VENDOR_SOURCE
        ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi/fsl_flexspi.c
    )
    add_library(
        ncr2_mcux_flexspi_adapter
        OBJECT
        platform/ncr2/flexspi/ncr2_flexspi_nor.c
        ${NCR2_FLEXSPI_VENDOR_SOURCE}
    )
    target_include_directories(
        ncr2_mcux_flexspi_adapter
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/flexspi
    )
    target_include_directories(
        ncr2_mcux_flexspi_adapter
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi
    )
    target_compile_definitions(
        ncr2_mcux_flexspi_adapter
        PRIVATE
            CPU_MIMXRT1051DVL6B
    )
    target_compile_options(
        ncr2_mcux_flexspi_adapter
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
    )
    set_source_files_properties(
        ${NCR2_FLEXSPI_VENDOR_SOURCE}
        PROPERTIES COMPILE_OPTIONS "-w"
    )

    add_executable(
        ncr2_flexspi_link_probe
        EXCLUDE_FROM_ALL
        platform/ncr2/flexspi/ncr2_flexspi_nor.c
        platform/ncr2/src/ncr2_nor.c
        ${NCR2_FLEXSPI_VENDOR_SOURCE}
    )
    target_include_directories(
        ncr2_flexspi_link_probe
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/flexspi
    )
    target_include_directories(
        ncr2_flexspi_link_probe
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi
    )
    target_compile_definitions(
        ncr2_flexspi_link_probe
        PRIVATE
            CPU_MIMXRT1051DVL6B
    )
    target_compile_options(
        ncr2_flexspi_link_probe
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
    )
    target_link_options(
        ncr2_flexspi_link_probe
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -nostdlib
            -Wl,-n
            -Wl,--gc-sections
            -Wl,--build-id=none
            -Wl,-e,ncr2_flexspi_nor_init
            -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ncr2_flexspi_link_probe.map
            -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/ncr2_bootloader.ld
    )
    set_target_properties(
        ncr2_flexspi_link_probe
        PROPERTIES SUFFIX ".elf"
    )
endif()

if(NCR2_BUILD_MCUX_BOARD_ADAPTER)
    if(NOT EXISTS
       "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/MIMXRT1051.h")
        message(
            FATAL_ERROR
            "NCR2_MCUX_SDK_ROOT is not a pinned MCUX SDK workspace"
        )
    endif()

    add_library(
        ncr2_mcux_board_adapter
        OBJECT
        platform/ncr2/board/ncr2_board.c
    )
    target_include_directories(
        ncr2_mcux_board_adapter
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/board
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/usb
    )
    target_include_directories(
        ncr2_mcux_board_adapter
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio
            ${NCR2_MCUX_SDK_ROOT}/core/components/lists
            ${NCR2_MCUX_SDK_ROOT}/core/components/osa
            ${NCR2_MCUX_SDK_ROOT}/middleware/usb/include
            ${NCR2_MCUX_SDK_ROOT}/middleware/usb/phy
    )
    target_compile_definitions(
        ncr2_mcux_board_adapter
        PRIVATE
            CPU_MIMXRT1051DVL6B
    )
    target_compile_options(
        ncr2_mcux_board_adapter
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
    )
endif()

if(NCR2_BUILD_MCUX_USB_ADAPTER AND
   NCR2_BUILD_MCUX_FLEXSPI_ADAPTER AND
   NCR2_BUILD_MCUX_BOARD_ADAPTER)
    set(
        NCR2_BOARD_VENDOR_SOURCES
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers/fsl_clock.c
        ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio/fsl_gpio.c
    )
    add_executable(
        ncr2_hardware_link_probe
        EXCLUDE_FROM_ALL
        bootloader/src/boot_journal.c
        bootloader/src/boot_recovery_request.c
        bootloader/src/boot_state.c
        bootloader/src/crc32.c
        bootloader/src/recovery_engine.c
        bootloader/src/recovery_protocol.c
        bootloader/src/recovery_storage.c
        bootloader/src/sha256.c
        platform/ncr2/board/ncr2_board.c
        platform/ncr2/flexspi/ncr2_flexspi_nor.c
        platform/ncr2/src/ncr2_nor.c
        platform/ncr2/usb/recovery_usb.c
        platform/ncr2/usb/recovery_usb_descriptor.c
        ${NCR2_BOARD_VENDOR_SOURCES}
        ${NCR2_FLEXSPI_VENDOR_SOURCE}
        ${NCR2_USB_VENDOR_SOURCES}
    )
    target_include_directories(
        ncr2_hardware_link_probe
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/board
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/flexspi
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/usb
    )
    target_include_directories(
        ncr2_hardware_link_probe
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio
            ${NCR2_MCUX_SDK_ROOT}/core/components/lists
            ${NCR2_MCUX_SDK_ROOT}/core/components/osa
            ${NCR2_USB_MIDDLEWARE}/include
            ${NCR2_USB_MIDDLEWARE}/device
            ${NCR2_USB_MIDDLEWARE}/phy
            ${NCR2_USB_OUTPUT}
            ${NCR2_USB_OUTPUT}/class
    )
    target_compile_definitions(
        ncr2_hardware_link_probe
        PRIVATE
            CPU_MIMXRT1051DVL6B
            USB_STACK_BM
            FSL_OSA_BM_TASK_ENABLE=0
            FSL_OSA_BM_TIMER_CONFIG=0
            SDK_OS_BAREMETAL
            NCR2_OPEN_USB_VID=0
            NCR2_OPEN_USB_PID=0
    )
    target_compile_options(
        ncr2_hardware_link_probe
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fdata-sections
            -ffunction-sections
            -fno-common
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wundef
    )
    set_source_files_properties(
        ${NCR2_BOARD_VENDOR_SOURCES}
        ${NCR2_FLEXSPI_VENDOR_SOURCE}
        ${NCR2_USB_VENDOR_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w"
    )
    target_link_options(
        ncr2_hardware_link_probe
        PRIVATE
            ${NCR2_CPU_FLAGS}
            -nostdlib
            -Wl,-n
            -Wl,--gc-sections
            -Wl,--build-id=none
            -Wl,-e,ncr2_board_recovery_input_init
            -Wl,--undefined=ncr2_board_usb_clock_init
            -Wl,--undefined=ncr2_board_usb_irq_enable
            -Wl,--undefined=ncr2_board_warm_reset
            -Wl,--undefined=ncr2_board_make_recovery_request
            -Wl,--undefined=ncr2_board_recovery_requested
            -Wl,--undefined=ncr2_recovery_usb_start
            -Wl,--undefined=ncr2_flexspi_nor_init
            -Wl,--undefined=USB_OTG1_IRQHandler
            -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ncr2_hardware_link_probe.map
            -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/ncr2_bootloader.ld
    )
    set_target_properties(
        ncr2_hardware_link_probe
        PROPERTIES SUFFIX ".elf"
    )
endif()
