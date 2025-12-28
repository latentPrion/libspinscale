cmake_minimum_required(VERSION 3.16)
project(libspinscale VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug FORCE)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")

# Debug options - allow parent to override when used as subdirectory
# option() will respect existing cache values, so parent can set them before add_subdirectory()
option(ENABLE_DEBUG_LOCKS "Enable debug features for locking system" OFF)
option(ENABLE_DEBUG_TRACE_CALLABLES
	"Enable callable tracing for debugging boost::asio post operations" OFF)

# Qutex deadlock detection configuration
if(NOT DEFINED DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS)
	set(DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS 500 CACHE STRING
		"Timeout in milliseconds for deadlock detection in qutex system")
endif()

if(ENABLE_DEBUG_LOCKS)
	# Validate the timeout value
	if(NOT DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS OR DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS STREQUAL "")
		message(FATAL_ERROR "DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS must be a positive integer > 0")
	endif()

	# Convert to integer and validate
	math(EXPR timeout_int "${DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS}")
	if(timeout_int LESS_EQUAL 0)
		message(FATAL_ERROR "DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS must be a positive integer > 0")
	endif()
endif()

# Set config variables for config.h
if(ENABLE_DEBUG_LOCKS)
	set(CONFIG_ENABLE_DEBUG_LOCKS TRUE)
endif()

if(ENABLE_DEBUG_TRACE_CALLABLES)
	set(CONFIG_DEBUG_TRACE_CALLABLES TRUE)
	# Suppress frame-address warnings when using __builtin_return_address()
	# with values above 0 (See callableTracer.h).
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-frame-address")
endif()

set(CONFIG_DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS ${DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS})

# Configure config.h
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/include/config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/include/config.h
	@ONLY
)

# Find dependencies
# Tell CMake we're linking against the shared library (not header-only)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_HEADER_ONLY OFF)
find_package(Boost REQUIRED COMPONENTS system log)
# Define BOOST_ALL_DYN_LINK project-wide to ensure all Boost libraries use dynamic linking
add_compile_definitions(BOOST_ALL_DYN_LINK)

find_package(Threads REQUIRED)

# Create the library
add_library(spinscale SHARED
	src/qutex.cpp
	src/lockerAndInvokerBase.cpp
	src/componentThread.cpp
	src/component.cpp
	src/puppetApplication.cpp
)

# Conditionally add qutexAcquisitionHistoryTracker.cpp only when debug locks
# are enabled, since the tracker is only referenced under CONFIG_ENABLE_DEBUG_LOCKS.
if(ENABLE_DEBUG_LOCKS)
	target_sources(spinscale PRIVATE src/qutexAcquisitionHistoryTracker.cpp)
endif()

# Set compile features
target_compile_features(spinscale PUBLIC cxx_std_20)

# Include directories
target_include_directories(spinscale PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
	$<INSTALL_INTERFACE:include>
)

# Link against required dependencies for shared library
# Boost::system is PUBLIC because componentThread.h exposes Boost.Asio types
target_link_libraries(spinscale PUBLIC
	Threads::Threads
	Boost::system
	Boost::log
)

# Verify Boost dynamic dependencies after build
# Prefer parent project's script when used as subdirectory, fall back to our own for standalone builds
set(VERIFY_SCRIPT "")
if(EXISTS ${CMAKE_SOURCE_DIR}/cmake/VerifyBoostDynamic.cmake)
	set(VERIFY_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/VerifyBoostDynamic.cmake)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBoostDynamic.cmake)
	set(VERIFY_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBoostDynamic.cmake)
endif()

if(VERIFY_SCRIPT)
	add_custom_command(TARGET spinscale POST_BUILD
		COMMAND ${CMAKE_COMMAND} -DVERIFY_FILE="$<TARGET_FILE:spinscale>"
			-P ${VERIFY_SCRIPT}
		COMMENT "Verifying Boost dynamic dependencies for spinscale"
	)
else()
	message(WARNING "VerifyBoostDynamic.cmake not found - cannot verify Boost dependencies for spinscale")
endif()

# Install rules
install(TARGETS spinscale
	EXPORT spinscaleTargets
	LIBRARY DESTINATION lib
	ARCHIVE DESTINATION lib
	RUNTIME DESTINATION bin
)

install(DIRECTORY include/spinscale
	DESTINATION include
	FILES_MATCHING PATTERN "*.h"
)

install(FILES include/boostAsioLinkageFix.h
	DESTINATION include
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/config.h
	DESTINATION include
)

# Install CMake config files for find_package() support
install(EXPORT spinscaleTargets
	FILE spinscaleTargets.cmake
	NAMESPACE spinscale::
	DESTINATION lib/cmake/spinscale
)

# Create config file for find_package()
include(CMakePackageConfigHelpers)

configure_package_config_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/spinscaleConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/spinscaleConfig.cmake
	INSTALL_DESTINATION lib/cmake/spinscale
)

write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/spinscaleConfigVersion.cmake
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)

install(FILES
	${CMAKE_CURRENT_BINARY_DIR}/spinscaleConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/spinscaleConfigVersion.cmake
	DESTINATION lib/cmake/spinscale
)
