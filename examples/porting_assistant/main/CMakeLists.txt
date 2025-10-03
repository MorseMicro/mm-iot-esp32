set(src "src/porting_assistant.c"
        "src/sdio_spi.c"
        "src/test_os.c"
        "src/test_wlan_fw_bcf.c"
        "src/test_wlan_io.c"
        "src/chip_cfg.c")

idf_component_register(SRCS ${src} PRIV_REQUIRES driver morselib mm_shims)
