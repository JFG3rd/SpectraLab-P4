idf_component_register(
    SRCS
        "src/qr_scan.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        esp_system
        esp_timer
        esp_video
        quirc
        freertos
        driver       # legacy i2c_param_config() — reclaims the shared GPIO 7/8 pads
)
