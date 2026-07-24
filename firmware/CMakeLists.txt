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
option(
    NCR2_BUILD_HARDWARE_BOOTLOADER
    "Build the unapproved bootable RT1051 recovery integration"
    OFF
)
option(
    NCR2_BUILD_RAM_RECOVERY
    "Build the SDRAM-resident whole-flash recovery application"
    OFF
)
option(
    NCR2_EMBED_RAM_RECOVERY
    "Embed and launch the SDRAM recovery image from the hardware bootloader"
    OFF
)
option(
    NCR2_BUILD_FACTORY_SLOT_APP
    "Build the unapproved source app for the recovered factory slot ABI"
    OFF
)
option(
    NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH
    "Enable the recovered unapproved SAI1/eDMA factory-slot passthrough"
    OFF
)
option(
    NCR2_FACTORY_SLOT_BOARD_CONTROLS
    "Enable recovered unapproved factory-slot GPIO startup levels"
    OFF
)
if(NCR2_FACTORY_SLOT_BOARD_CONTROLS AND
   NOT NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH)
    message(
        FATAL_ERROR
        "factory-slot board controls require the audio passthrough"
    )
endif()
option(
    NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
    "Enable physical NOR mutation in the unapproved hardware bootloader"
    OFF
)
option(
    NCR2_ENABLE_HARDWARE_USB_ENUMERATION
    "Allow the hardware bootloader to enumerate with assigned USB IDs"
    OFF
)
option(
    NCR2_ALLOW_BORROWED_NUX_DFU_ID
    "Bench-only override allowing the stock NUX 9527:c157 USB identity"
    OFF
)
set(
    NCR2_OPEN_USB_VID
    "0"
    CACHE STRING
    "Assigned open-firmware USB VID; ignored unless enumeration is enabled"
)
set(
    NCR2_OPEN_USB_PID
    "0"
    CACHE STRING
    "Assigned open-firmware USB PID; ignored unless enumeration is enabled"
)
set(
    NCR2_MCUX_SDK_ROOT
    ""
    CACHE PATH
    "Pinned workspace produced by tools/fetch_mcux_sdk.py"
)

set(NCR2_EFFECTIVE_USB_VID 0)
set(NCR2_EFFECTIVE_USB_PID 0)
set(NCR2_BORROWED_NUX_DFU_ID_VALUE 0)
if(NCR2_ALLOW_BORROWED_NUX_DFU_ID)
    set(NCR2_BORROWED_NUX_DFU_ID_VALUE 1)
endif()
if(NCR2_ENABLE_HARDWARE_USB_ENUMERATION)
    math(EXPR NCR2_USB_VID_VALUE "${NCR2_OPEN_USB_VID}")
    math(EXPR NCR2_USB_PID_VALUE "${NCR2_OPEN_USB_PID}")
    if(NCR2_USB_VID_VALUE LESS 1 OR
       NCR2_USB_VID_VALUE GREATER 65535 OR
       NCR2_USB_PID_VALUE LESS 1 OR
       NCR2_USB_PID_VALUE GREATER 65535)
        message(
            FATAL_ERROR
            "USB enumeration requires nonzero 16-bit assigned VID/PID"
        )
    endif()
    if(NCR2_USB_VID_VALUE EQUAL 38183 AND
       NOT (NCR2_ALLOW_BORROWED_NUX_DFU_ID AND
            NCR2_USB_PID_VALUE EQUAL 49495))
        message(
            FATAL_ERROR
            "NUX VID 0x9527 is forbidden for open firmware unless the "
            "bench-only borrowed 9527:c157 override is explicit"
        )
    endif()
    set(NCR2_EFFECTIVE_USB_VID ${NCR2_USB_VID_VALUE})
    set(NCR2_EFFECTIVE_USB_PID ${NCR2_USB_PID_VALUE})
endif()

set(NCR2_HARDWARE_WRITE_VALUE 0)
if(NCR2_HARDWARE_RECOVERY_WRITE_ENABLE)
    set(NCR2_HARDWARE_WRITE_VALUE 1)
endif()
set(NCR2_EMBED_RAM_RECOVERY_VALUE 0)
if(NCR2_EMBED_RAM_RECOVERY)
    set(NCR2_EMBED_RAM_RECOVERY_VALUE 1)
endif()

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
    bootloader/src/boot_handoff.c
    bootloader/src/boot_journal.c
    bootloader/src/boot_lifecycle.c
    bootloader/src/boot_recovery_request.c
    bootloader/src/bootloader.c
    bootloader/src/bootloader_offline.c
    bootloader/src/boot_state.c
    bootloader/src/boot_trial.c
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
    app/src/effect_runtime.c
    app/src/effects_basic.c
    app/src/main.c
    app/src/program_runtime.c
    app/src/program_selector.c
    app/src/programs_builtin.c
    bootloader/src/boot_recovery_request.c
    bootloader/src/boot_trial.c
    platform/ncr2/src/ncr2_boot_request.c
)
target_include_directories(
    ncr2_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
)
ncr2_firmware_target(
    ncr2_app
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ncr2_app.ld
)

add_executable(
    ncr2_factory_bridge
    EXCLUDE_FROM_ALL
    factory_bridge/src/startup.S
    bootloader/src/boot_trial.c
)
target_include_directories(
    ncr2_factory_bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
)
ncr2_firmware_target(
    ncr2_factory_bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ncr2_app.ld
)

if(EXISTS
   "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/MIMXRT1051.h")
    set(
        NCR2_FACTORY_COMPAT_CLOCK_SOURCE
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers/fsl_clock.c
    )
    foreach(
        target
        IN ITEMS
            ncr2_factory_compat_bridge
            ncr2_factory_compat_diagnostic
    )
        add_executable(
            ${target}
            EXCLUDE_FROM_ALL
            factory_bridge/src/startup.S
            factory_bridge/src/board_compat.c
            bootloader/src/boot_trial.c
            ${NCR2_FACTORY_COMPAT_CLOCK_SOURCE}
        )
        target_include_directories(
            ${target}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
        )
        target_include_directories(
            ${target}
            SYSTEM PRIVATE
                ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
        )
        target_compile_definitions(
            ${target}
            PRIVATE
                CPU_MIMXRT1051DVL6B
                NCR2_FACTORY_COMPAT_PREPARE=1
        )
        set_source_files_properties(
            ${NCR2_FACTORY_COMPAT_CLOCK_SOURCE}
            PROPERTIES COMPILE_OPTIONS "-w"
        )
        ncr2_firmware_target(
            ${target}
            ${CMAKE_CURRENT_SOURCE_DIR}/app/ncr2_app.ld
        )
    endforeach()
    target_compile_definitions(
        ncr2_factory_compat_diagnostic
        PRIVATE
            NCR2_FACTORY_COMPAT_DIAGNOSTIC=1
    )
endif()

if(NCR2_BUILD_FACTORY_SLOT_APP)
    if(NOT EXISTS
       "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/system_MIMXRT1051.c")
        message(
            FATAL_ERROR
            "NCR2_BUILD_FACTORY_SLOT_APP requires the pinned MCUX SDK workspace"
        )
    endif()

    set(
        NCR2_FACTORY_SLOT_SYSTEM_SOURCE
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/system_MIMXRT1051.c
    )
    set(
        NCR2_FACTORY_SLOT_CLOCK_SOURCE
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers/fsl_clock.c
    )
    add_executable(
        ncr2_factory_slot_app
        EXCLUDE_FROM_ALL
        factory_slot/src/startup.S
        factory_slot/src/audio_passthrough.c
        factory_slot/src/board_control.c
        factory_slot/src/main.c
        app/src/effect_runtime.c
        app/src/effects_basic.c
        app/src/program_runtime.c
        app/src/program_selector.c
        app/src/programs_builtin.c
        ${NCR2_FACTORY_SLOT_SYSTEM_SOURCE}
        ${NCR2_FACTORY_SLOT_CLOCK_SOURCE}
    )
    target_include_directories(
        ncr2_factory_slot_app
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/app/include
            ${CMAKE_CURRENT_SOURCE_DIR}/factory_slot/include
    )
    target_include_directories(
        ncr2_factory_slot_app
        SYSTEM PRIVATE
            ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
            ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
    )
    target_compile_definitions(
        ncr2_factory_slot_app
        PRIVATE
            CPU_MIMXRT1051DVL6B
            NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH=$<BOOL:${NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH}>
            NCR2_FACTORY_SLOT_BOARD_CONTROLS=$<BOOL:${NCR2_FACTORY_SLOT_BOARD_CONTROLS}>
    )
    set_source_files_properties(
        ${NCR2_FACTORY_SLOT_SYSTEM_SOURCE}
        ${NCR2_FACTORY_SLOT_CLOCK_SOURCE}
        PROPERTIES COMPILE_OPTIONS "-w"
    )
    ncr2_firmware_target(
        ncr2_factory_slot_app
        ${CMAKE_CURRENT_SOURCE_DIR}/factory_slot/ncr2_factory_slot.ld
    )
    target_link_options(ncr2_factory_slot_app PRIVATE -Wl,-n)
endif()

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
        ${NCR2_MCUX_SDK_ROOT}/core/components/osa/fsl_os_abstraction_bm.c
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
            DATA_SECTION_IS_CACHEABLE=0
            NCR2_OPEN_USB_VID=${NCR2_EFFECTIVE_USB_VID}
            NCR2_OPEN_USB_PID=${NCR2_EFFECTIVE_USB_PID}
            NCR2_ALLOW_BORROWED_NUX_DFU_ID=${NCR2_BORROWED_NUX_DFU_ID_VALUE}
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
        platform/ncr2/src/ncr2_freestanding.c
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
        platform/ncr2/board/ncr2_watchdog.c
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
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/wdog01
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
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/system_MIMXRT1051.c
        ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers/fsl_clock.c
        ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio/fsl_gpio.c
        ${NCR2_MCUX_SDK_ROOT}/core/drivers/wdog01/fsl_wdog.c
    )
    add_executable(
        ncr2_hardware_link_probe
        EXCLUDE_FROM_ALL
        bootloader/src/boot_journal.c
        bootloader/src/boot_handoff.c
        bootloader/src/boot_recovery_request.c
        bootloader/src/boot_state.c
        bootloader/src/boot_trial.c
        bootloader/src/crc32.c
        bootloader/src/recovery_engine.c
        bootloader/src/recovery_protocol.c
        bootloader/src/recovery_storage.c
        bootloader/src/sha256.c
        platform/ncr2/board/ncr2_board.c
        platform/ncr2/board/ncr2_watchdog.c
        platform/ncr2/flexspi/ncr2_flexspi_nor.c
        platform/ncr2/src/ncr2_freestanding.c
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
            ${NCR2_MCUX_SDK_ROOT}/core/drivers/wdog01
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
            DATA_SECTION_IS_CACHEABLE=0
            NCR2_OPEN_USB_VID=${NCR2_EFFECTIVE_USB_VID}
            NCR2_OPEN_USB_PID=${NCR2_EFFECTIVE_USB_PID}
            NCR2_ALLOW_BORROWED_NUX_DFU_ID=${NCR2_BORROWED_NUX_DFU_ID_VALUE}
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
            -Wl,--undefined=ncr2_board_watchdog_start_trial
            -Wl,--undefined=ncr2_board_watchdog_refresh
            -Wl,--undefined=ncr2_board_watchdog_reset_status
            -Wl,--undefined=boot_handoff_prepare
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

    if(NCR2_BUILD_HARDWARE_BOOTLOADER)
        add_executable(
            ncr2_hardware_bootloader
            EXCLUDE_FROM_ALL
            bootloader/src/startup.S
            bootloader/src/boot_controller.c
            bootloader/src/boot_handoff.c
            bootloader/src/boot_journal.c
            bootloader/src/boot_lifecycle.c
            bootloader/src/boot_recovery_request.c
            bootloader/src/bootloader.c
            bootloader/src/bootloader_hardware.c
            bootloader/src/boot_state.c
            bootloader/src/boot_trial.c
            bootloader/src/crc32.c
            bootloader/src/recovery_engine.c
            bootloader/src/recovery_protocol.c
            bootloader/src/recovery_storage.c
            bootloader/src/sha256.c
            platform/ncr2/board/ncr2_board.c
            platform/ncr2/board/ncr2_watchdog.c
            platform/ncr2/flexspi/ncr2_flexspi_nor.c
            platform/ncr2/src/ncr2_freestanding.c
            platform/ncr2/src/ncr2_nor.c
            platform/ncr2/usb/recovery_usb.c
            platform/ncr2/usb/recovery_usb_descriptor.c
            ${NCR2_BOARD_VENDOR_SOURCES}
            ${NCR2_FLEXSPI_VENDOR_SOURCE}
            ${NCR2_USB_VENDOR_SOURCES}
        )
        target_include_directories(
            ncr2_hardware_bootloader
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/board
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/flexspi
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/usb
        )
        target_include_directories(
            ncr2_hardware_bootloader
            SYSTEM PRIVATE
                ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/wdog01
                ${NCR2_MCUX_SDK_ROOT}/core/components/lists
                ${NCR2_MCUX_SDK_ROOT}/core/components/osa
                ${NCR2_USB_MIDDLEWARE}/include
                ${NCR2_USB_MIDDLEWARE}/device
                ${NCR2_USB_MIDDLEWARE}/phy
                ${NCR2_USB_OUTPUT}
                ${NCR2_USB_OUTPUT}/class
        )
        target_compile_definitions(
            ncr2_hardware_bootloader
            PRIVATE
                CPU_MIMXRT1051DVL6B
                USB_STACK_BM
                FSL_OSA_BM_TASK_ENABLE=0
                FSL_OSA_BM_TIMER_CONFIG=0
                SDK_OS_BAREMETAL
                DATA_SECTION_IS_CACHEABLE=0
                NCR2_HARDWARE_STARTUP=1
                NCR2_OPEN_USB_VID=${NCR2_EFFECTIVE_USB_VID}
                NCR2_OPEN_USB_PID=${NCR2_EFFECTIVE_USB_PID}
                NCR2_ALLOW_BORROWED_NUX_DFU_ID=${NCR2_BORROWED_NUX_DFU_ID_VALUE}
                NCR2_HARDWARE_RECOVERY_WRITE_ENABLE=${NCR2_HARDWARE_WRITE_VALUE}
                NCR2_EMBED_RAM_RECOVERY=${NCR2_EMBED_RAM_RECOVERY_VALUE}
        )
        ncr2_firmware_target(
            ncr2_hardware_bootloader
            ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/ncr2_bootloader.ld
        )
        target_link_options(
            ncr2_hardware_bootloader
            PRIVATE
                -Wl,-n
                -Wl,--undefined=USB_OTG1_IRQHandler
        )
        if(NCR2_HARDWARE_RECOVERY_WRITE_ENABLE)
            target_link_options(
                ncr2_hardware_bootloader
                PRIVATE
                    -Wl,--undefined=ncr2_hardware_recovery_write_enabled
            )
        else()
            target_link_options(
                ncr2_hardware_bootloader
                PRIVATE
                    -Wl,--undefined=ncr2_hardware_recovery_readonly
            )
        endif()
    endif()

    if(NCR2_BUILD_RAM_RECOVERY)
        add_executable(
            ncr2_ram_recovery
            EXCLUDE_FROM_ALL
            ram_recovery/src/startup.S
            ram_recovery/src/main.c
            bootloader/src/boot_journal.c
            bootloader/src/boot_state.c
            bootloader/src/crc32.c
            bootloader/src/recovery_engine.c
            bootloader/src/recovery_protocol.c
            bootloader/src/recovery_storage.c
            bootloader/src/sha256.c
            platform/ncr2/board/ncr2_board.c
            platform/ncr2/board/ncr2_watchdog.c
            platform/ncr2/flexspi/ncr2_flexspi_nor.c
            platform/ncr2/src/ncr2_freestanding.c
            platform/ncr2/src/ncr2_nor.c
            platform/ncr2/usb/recovery_usb.c
            platform/ncr2/usb/recovery_usb_descriptor.c
            ${NCR2_BOARD_VENDOR_SOURCES}
            ${NCR2_FLEXSPI_VENDOR_SOURCE}
            ${NCR2_USB_VENDOR_SOURCES}
        )
        target_include_directories(
            ncr2_ram_recovery
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/include
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/board
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/flexspi
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/usb
        )
        target_include_directories(
            ncr2_ram_recovery
            SYSTEM PRIVATE
                ${NCR2_MCUX_SDK_ROOT}/core/CMSIS/Core/Include
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051
                ${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/drivers
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/common
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/flexspi
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/igpio
                ${NCR2_MCUX_SDK_ROOT}/core/drivers/wdog01
                ${NCR2_MCUX_SDK_ROOT}/core/components/lists
                ${NCR2_MCUX_SDK_ROOT}/core/components/osa
                ${NCR2_USB_MIDDLEWARE}/include
                ${NCR2_USB_MIDDLEWARE}/device
                ${NCR2_USB_MIDDLEWARE}/phy
                ${NCR2_USB_OUTPUT}
                ${NCR2_USB_OUTPUT}/class
        )
        target_compile_definitions(
            ncr2_ram_recovery
            PRIVATE
                CPU_MIMXRT1051DVL6B
                USB_STACK_BM
                FSL_OSA_BM_TASK_ENABLE=0
                FSL_OSA_BM_TIMER_CONFIG=0
                SDK_OS_BAREMETAL
                DATA_SECTION_IS_CACHEABLE=0
                NCR2_OPEN_USB_VID=${NCR2_EFFECTIVE_USB_VID}
                NCR2_OPEN_USB_PID=${NCR2_EFFECTIVE_USB_PID}
                NCR2_ALLOW_BORROWED_NUX_DFU_ID=${NCR2_BORROWED_NUX_DFU_ID_VALUE}
        )
        ncr2_firmware_target(
            ncr2_ram_recovery
            ${CMAKE_CURRENT_SOURCE_DIR}/ram_recovery/ncr2_ram_recovery.ld
        )
        target_link_options(
            ncr2_ram_recovery
            PRIVATE
                -Wl,-n
                -Wl,--undefined=USB_OTG1_IRQHandler
        )
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_custom_command(
            TARGET ncr2_ram_recovery
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_ram_recovery.py
                $<TARGET_FILE:ncr2_ram_recovery>
                ${CMAKE_CURRENT_BINARY_DIR}/ncr2_ram_recovery.bin
            VERBATIM
        )
    endif()

    if(NCR2_EMBED_RAM_RECOVERY)
        set(
            NCR2_RAM_RECOVERY_BLOB_OBJECT
            ${CMAKE_CURRENT_BINARY_DIR}/ncr2_ram_recovery_blob.o
        )
        add_custom_command(
            OUTPUT ${NCR2_RAM_RECOVERY_BLOB_OBJECT}
            COMMAND
                ${CMAKE_OBJCOPY}
                -I binary
                -O elf32-littlearm
                -B arm
                --rename-section
                .data=.ram_recovery_blob,alloc,load,readonly,data,contents
                ncr2_ram_recovery.bin
                ncr2_ram_recovery_blob.o
            DEPENDS ncr2_ram_recovery
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            VERBATIM
        )
        add_custom_target(
            ncr2_ram_recovery_blob
            DEPENDS ${NCR2_RAM_RECOVERY_BLOB_OBJECT}
        )
        set_source_files_properties(
            ${NCR2_RAM_RECOVERY_BLOB_OBJECT}
            PROPERTIES
                EXTERNAL_OBJECT TRUE
                GENERATED TRUE
        )
        target_sources(
            ncr2_hardware_bootloader
            PRIVATE ${NCR2_RAM_RECOVERY_BLOB_OBJECT}
        )
        add_dependencies(
            ncr2_hardware_bootloader
            ncr2_ram_recovery_blob
        )
    endif()
endif()

if(NCR2_BUILD_HARDWARE_BOOTLOADER AND
   NOT (NCR2_BUILD_MCUX_USB_ADAPTER AND
        NCR2_BUILD_MCUX_FLEXSPI_ADAPTER AND
        NCR2_BUILD_MCUX_BOARD_ADAPTER))
    message(
        FATAL_ERROR
        "NCR2_BUILD_HARDWARE_BOOTLOADER requires all three MCUX adapters"
    )
endif()

if(NCR2_BUILD_RAM_RECOVERY AND
   NOT (NCR2_BUILD_MCUX_USB_ADAPTER AND
        NCR2_BUILD_MCUX_FLEXSPI_ADAPTER AND
        NCR2_BUILD_MCUX_BOARD_ADAPTER))
    message(
        FATAL_ERROR
        "NCR2_BUILD_RAM_RECOVERY requires all three MCUX adapters"
    )
endif()

if(NCR2_EMBED_RAM_RECOVERY AND
   NOT (NCR2_BUILD_HARDWARE_BOOTLOADER AND
        NCR2_BUILD_RAM_RECOVERY))
    message(
        FATAL_ERROR
        "NCR2_EMBED_RAM_RECOVERY requires both the hardware bootloader "
        "and RAM recovery targets"
    )
endif()
