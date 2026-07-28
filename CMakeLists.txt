cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_chip_info)

add_subdirectory(drivers)

target_sources(app PRIVATE
	src/main_ec801e.c
	src/bitproto/drone_bp.c
	src/bitproto/frame_header_bp.c
)
