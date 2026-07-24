cmake_minimum_required(VERSION 3.24)

project(nux_effects_firmware LANGUAGES C ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
    bootloader/src/bootloader.c
    bootloader/src/sha256.c
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

add_executable(
    ncr2_app
    app/src/startup.S
    app/src/main.c
)
target_include_directories(
    ncr2_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/ncr2/include
)
ncr2_firmware_target(
    ncr2_app
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ncr2_app.ld
)
