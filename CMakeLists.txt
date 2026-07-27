cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_chip_info)

target_sources(app PRIVATE src/main_chip_info.c src/epd200x200.c)
