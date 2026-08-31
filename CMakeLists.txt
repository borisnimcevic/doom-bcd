# BalCCon Cyberdeck 0o27 — DOOM (doomgeneric) port
cmake_minimum_required(VERSION 3.16)

# doomgeneric renders at DOOM's native 320x200; we downscale on flush.
add_compile_definitions(DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doom-bcd)

# Build a SPIFFS image from flash_data/ (contains doom1.wad) into the
# "storage" partition; `idf.py flash` writes it alongside the app.
spiffs_create_partition_image(storage flash_data FLASH_IN_PROJECT)
