set(inc "include")

idf_component_register(INCLUDE_DIRS ${inc}
                       WHOLE_ARCHIVE)
if(${IDF_TARGET} STREQUAL "esp32s3")
    add_prebuilt_library(libmorse "lib/esp32s3/libmorse_nocrypto.a"
                         PRIV_REQUIRES esp_rom mmpktmem)
elseif(${IDF_TARGET} STREQUAL "esp32c6")
    add_prebuilt_library(libmorse "lib/esp32c6/libmorse_nocrypto.a"
                         PRIV_REQUIRES esp_rom mmpktmem)
elseif(${IDF_TARGET} STREQUAL "esp32c3")
    add_prebuilt_library(libmorse "lib/esp32c3/libmorse_nocrypto.a"
                         PRIV_REQUIRES esp_rom mmpktmem)
else()
        message(FATAL_ERROR "ESP-IDF target ${IDF_TARGET} not supported, exiting.")
endif()

target_link_libraries(${COMPONENT_LIB} INTERFACE libmorse)
