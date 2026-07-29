cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_chip_info)

add_subdirectory(drivers)

target_sources(app PRIVATE
	src/main_ec801e_hid.c
	src/serv_addr.c
	src/bitproto/bitproto.c
	src/bitproto/drone_bp.c
	src/bitproto/frameheader_bp.c
)
