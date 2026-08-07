cmake_minimum_required(VERSION 3.16.0)

# Add project components directory so pure ESP-IDF cmake builds (idf.py build)
# can find our custom components.  PlatformIO auto-detects components/ at the
# project root, so no extra configuration is needed for PlatformIO builds.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

# Explicit firmware release version used by esp_app_get_description()->version.
set(PROJECT_VER "1.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SpectraLab-P4)

# ── esp_ipa: strip the stale dummy sensor-tuning object ──────────────────────
#
# espressif__esp_ipa's prebuilt lib/<target>/<idf>/libesp_ipa.a ships an
# esp_video_ipa_config.c.obj built from Espressif's own `test_apps_dummy` JSON.
# It defines the same esp_ipa_pipeline_get_config() as the config generated
# from the real sensor JSONs, and the component's CMakeLists deliberately
# creates a circular link (`target_link_libraries(prebuilt INTERFACE
# ${COMPONENT_LIB})`) so the archives repeat on the link line — which lets the
# prebuilt's copy win. The lookup then returns NULL for every sensor,
# esp_video logs "failed to get configuration to initialize ISP controller",
# white-balance gains are never set and VIDIOC_STREAMON fails.
#
# This MUST run here rather than from tools/generate_esp_ipa_config.py: the
# component manager repopulates managed_components/ during project(), which
# undoes any strip done earlier at PlatformIO script-parse time. Running after
# project() puts it after the restore and before the link.
#
# Idempotent — on later configures the member is already gone.
file(GLOB _ipa_prebuilt_libs
     "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_ipa/lib/*/*/libesp_ipa.a")
foreach(_ipa_lib ${_ipa_prebuilt_libs})
    execute_process(COMMAND "${CMAKE_AR}" t "${_ipa_lib}"
                    OUTPUT_VARIABLE _ipa_members ERROR_QUIET)
    if(_ipa_members MATCHES "esp_video_ipa_config\\.c\\.obj")
        execute_process(COMMAND "${CMAKE_AR}" d "${_ipa_lib}" esp_video_ipa_config.c.obj)
        message(STATUS "esp_ipa: stripped stale esp_video_ipa_config.c.obj from ${_ipa_lib}")
    endif()
endforeach()
