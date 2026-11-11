# SPDX-License-Identifier: MIT
# wiilib: the Wii libraries melee's Wii port uses.
# Wii only (the devkitPro toolchain, libogc).
#   wl_base     - results, logging (wl/base: base.c, log.c)
#   wl_gcm      - the GameCube disc image reader (wl/gcm)
#   wl_platform - files on SD and USB, the log's sink, the reset and power buttons, leaving and the
#                 stop screen (wl/platform)
set(_wiilib_warnings -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wundef)

add_library(wl_base STATIC wl/base/base.c wl/base/log.c)
target_include_directories(wl_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(wl_base PRIVATE -std=gnu11 ${_wiilib_warnings} -ffp-contract=off)
add_library(wiilib::base ALIAS wl_base)

add_library(wl_gcm STATIC wl/gcm/gcm.c)
target_compile_options(wl_gcm PRIVATE -std=gnu11 ${_wiilib_warnings})
target_link_libraries(wl_gcm PUBLIC wl_base)
add_library(wiilib::gcm ALIAS wl_gcm)

add_library(wl_platform STATIC
    wl/platform/wii/fs_wii.c
    wl/platform/wii/log_wii.c
    wl/platform/wii/stop_wii.c
    wl/platform/wii/system_wii.c)
target_compile_options(wl_platform PRIVATE -std=gnu11 ${_wiilib_warnings})
target_link_libraries(wl_platform PUBLIC wl_base fat ogc m)
add_library(wiilib::platform ALIAS wl_platform)
