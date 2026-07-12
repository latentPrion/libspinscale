# EXPLANATION:
# Shared Boost deps for standalone or nested libspinscale builds.
# Always require Boost.Log as a shared library. Require Boost.System only on
# versions that still ship a compiled stub (Ubuntu dropped it at Boost 1.89).
#
# Sets BOOST_SHARED_DEP_TARGETS for target_link_libraries(... ${BOOST_SHARED_DEP_TARGETS}).
# Uses Boost:: imported targets (not a project INTERFACE lib) so export sets stay valid.

set(BOOST_COMPILED_SYSTEM_REMOVED_VERSION "1.89.0")
set(BOOST_SHARED_DEPS_MIN_VERSION "1.69")

function(boostSharedDepsComponentsForVersion _boostVersion _outVar)
	set(_components log)
	if(_boostVersion VERSION_LESS "${BOOST_COMPILED_SYSTEM_REMOVED_VERSION}")
		list(APPEND _components system)
	endif()
	set(${_outVar} ${_components} PARENT_SCOPE)
endfunction()

function(boostSharedDepsLinkTargets _outVar)
	set(_targets Boost::log)
	if(TARGET Boost::system)
		list(APPEND _targets Boost::system)
	endif()
	set(${_outVar} ${_targets} PARENT_SCOPE)
endfunction()

function(boostSharedDepsReportStatus)
	if(TARGET Boost::system)
		message(STATUS
			"Boost ${Boost_VERSION}: linking Boost::system "
			"(compiled stub still present)")
	else()
		message(STATUS
			"Boost ${Boost_VERSION}: omitting Boost::system "
			"(header-only; compiled stub removed at "
			"${BOOST_COMPILED_SYSTEM_REMOVED_VERSION})")
	endif()
endfunction()

if(NOT BOOST_SHARED_DEPS_RESOLVED)
	# Prefer shared Boost libs where a compiled component still exists.
	set(Boost_USE_STATIC_LIBS OFF)
	set(Boost_USE_HEADER_ONLY OFF)

	# Resolve version before requesting components so we can skip system on
	# Boost 1.89+, where Ubuntu no longer packages libboost_system.
	find_package(Boost ${BOOST_SHARED_DEPS_MIN_VERSION} REQUIRED)
	boostSharedDepsComponentsForVersion("${Boost_VERSION}" _boostSharedDepsComponents)
	find_package(Boost ${BOOST_SHARED_DEPS_MIN_VERSION} REQUIRED
		COMPONENTS ${_boostSharedDepsComponents})

	boostSharedDepsLinkTargets(BOOST_SHARED_DEP_TARGETS)
	set(BOOST_SHARED_DEPS_RESOLVED TRUE)
	boostSharedDepsReportStatus()

	# Ensure remaining Boost libs (e.g. Log) use dynamic linking.
	add_compile_definitions(BOOST_ALL_DYN_LINK)
endif()
