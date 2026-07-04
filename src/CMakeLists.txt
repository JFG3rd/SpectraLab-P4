# Main application component
#
# Changed from GLOB to explicit SRCS:
#   - GLOB causes CMake not to re-run when new .c files are added
#   - Explicit list makes build reproducible
#
# REQUIRES lists components whose public headers main.c includes directly.
# PRIV_REQUIRES would list components used only in implementation (none here;
# all component APIs are orchestrated from main.c).

idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp32_p4_function_ev_board
        esp_lvgl_port
        audio_source
        dsp_engine
        display_ui
        settings_mgr
        nvs_flash
        esp_event
        esp_timer
)
