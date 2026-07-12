add_library(spinscale_probe_support STATIC
	probeComponentThread.cpp
)

target_include_directories(spinscale_probe_support PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(spinscale_probe_support PUBLIC
	spinscale
)
