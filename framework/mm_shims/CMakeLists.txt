# Copyright 2024 Morse Micro
# SPDX-License-Identifier: Apache-2.0
set(src "mmosal_shim_freertos_esp32.c"
        "mmhal.c"
        "mmhal_wlan_binaries.c"
        "wlan_hal.c"
        "crypto_mbedtls_mm.c")

# Define architecture specific paths and variables
if(${IDF_TARGET} STREQUAL "esp32s3")
    set(inc "include/esp32s3")
    set(BFDNAME "elf32-xtensa-le")
    set(ARCH "xtensa")
elseif(${IDF_TARGET} STREQUAL "esp32c6")
    set(inc "include/esp32c6")
    set(BFDNAME "elf32-littleriscv")
    set(ARCH "riscv")
elseif(${IDF_TARGET} STREQUAL "esp32c3")
    set(inc "include/esp32c3")
    set(BFDNAME "elf32-littleriscv")
    set(ARCH "riscv")
else()
    message(FATAL_ERROR "Target ${IDF_TARGET} not known. Exiting...")
endif()

idf_component_register(INCLUDE_DIRS ${inc}
                       SRCS ${src}
                       PRIV_REQUIRES morselib spi_flash app_update log driver mbedtls esp_timer
                       WHOLE_ARCHIVE)

# Kconfig variables are used to determine which bcf file to add to the final application. The file
# can be in one of two location.
#   - In `morsefirmware/` in the root of the application being built.
#   - In `framework/morsefirmware/` in the mm-iot-esp32 sdk.
set(APPLICATION_BLOB_DIR "${CMAKE_SOURCE_DIR}/morsefirmware")
set(MMIOT_BLOB_DIR "${CMAKE_CURRENT_LIST_DIR}/../morsefirmware")

function(find_binary_blob FILE_VAR OBJ_VAR CONFIG_VAR)
    # Clear the CMAKE cache to force it to search for the file every time.
    unset(${FILE_VAR} CACHE)

    find_file(${FILE_VAR}
        NAMES ${${CONFIG_VAR}}
        PATHS ${APPLICATION_BLOB_DIR} ${MMIOT_BLOB_DIR}
        NO_DEFAULT_PATH
    )

    if(NOT ${FILE_VAR})
        message(WARNING
            "Invalid ${FILE_VAR} specified: ${${CONFIG_VAR}}\n"
            "File must be present in one of the following:\n"
            " - ${APPLICATION_BLOB_DIR}\n"
            " - ${MMIOT_BLOB_DIR}"
        )
    endif()

    message(STATUS "${FILE_VAR}: ${${FILE_VAR}}")

    set(${OBJ_VAR} "${${FILE_VAR}}.o" PARENT_SCOPE)
endfunction()

find_binary_blob(BCF_MBIN BCF_OBJ CONFIG_MM_BCF_FILE)
find_binary_blob(FW_MBIN FW_OBJ CONFIG_MM_FW_FILE)

function(make_binary_object __file __prefix)
    get_filename_component(__file_name ${__file} NAME)
    set(__object ${CMAKE_CURRENT_LIST_DIR}/../morsefirmware/${__file_name}.o)
    string(MAKE_C_IDENTIFIER ${__file} __file_c_identifier)
    # message(FATAL_ERROR ${__object}; ${CMAKE_OBJCOPY})
    add_custom_command(
        OUTPUT ${__object}
        COMMAND ${CMAKE_OBJCOPY} -I binary -O ${BFDNAME} -B ${ARCH} ${__file} ${__object}
            --redefine-sym _binary_${__file_c_identifier}_start=${__prefix}_start
            --redefine-sym _binary_${__file_c_identifier}_size=${__prefix}_size
            --redefine-sym _binary_${__file_c_identifier}_end=${__prefix}_end
            --rename-section .data=.rodata._fw_mbin,contents,alloc,load,readonly,data
        DEPENDS ${__file}
        COMMENT "Running objcopy on ${__file} to generate ${__object}"
    )
endfunction()

make_binary_object(${FW_MBIN} firmware_binary)
make_binary_object(${BCF_MBIN} bcf_binary)
add_custom_target(morsefirmware ALL DEPENDS ${FW_OBJ} ${BCF_OBJ})

add_dependencies(${COMPONENT_TARGET} morsefirmware)

target_link_libraries(${COMPONENT_TARGET} PRIVATE
    "${FW_OBJ}"
    "${BCF_OBJ}"
)
