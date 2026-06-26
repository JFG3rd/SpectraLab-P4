cmake_minimum_required(VERSION 3.16.0)

# Add project components directory so pure ESP-IDF cmake builds (idf.py build)
# can find our custom components.  PlatformIO auto-detects components/ at the
# project root, so no extra configuration is needed for PlatformIO builds.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32-P4-Function-EV-Board-Getting-Started)
