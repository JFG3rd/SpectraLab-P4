#pragma once

/*
 * IntelliSense shim for ESP-IDF projects.
 *
 * PlatformIO generates the real sdkconfig header into:
 *   .pio/build/esp32-p4-evboard/config/sdkconfig.h
 *
 * VS Code can index headers before that file exists, so this shim keeps
 * include resolution working. When the generated header is present, we
 * include it directly.
 */
#if defined(__has_include)
#if __has_include("../.pio/build/esp32-p4-evboard/config/sdkconfig.h")
#include "../.pio/build/esp32-p4-evboard/config/sdkconfig.h"
#endif
#endif
