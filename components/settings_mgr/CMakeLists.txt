idf_component_register(
    SRCS "src/settings_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        log
        nvs_flash
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp32_p4_function_ev_board
        json
        dsp_engine
)
