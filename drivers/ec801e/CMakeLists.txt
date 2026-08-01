target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/ec801e.c
	${CMAKE_CURRENT_SOURCE_DIR}/ec801e_socket_offload.c
)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)
