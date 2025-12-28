# SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY
# Verifies that a target file (executable or shared library) has Boost libraries
# in its dynamic dependency list via ldd.
#
# Usage as function:
#   SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY(<target_file>)
#
# Usage as script (with -P):
#   cmake -DVERIFY_FILE=<target_file> -P VerifyBoostDynamic.cmake
#
# This function/script:
#   1. Runs ldd on the target file
#   2. Checks for boost libraries in the dependency list
#   3. Reports success or failure with appropriate messages
#
function(SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY target_file)
	_verify_boost_dynamic_dependency("${target_file}")
endfunction()

# Internal implementation that can be called from script mode or function mode
function(_verify_boost_dynamic_dependency target_file)
	if(NOT EXISTS "${target_file}")
		message(WARNING "SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY: Target file '${target_file}' does not exist")
		return()
	endif()

	# Run ldd on the target file
	execute_process(
		COMMAND ldd "${target_file}"
		OUTPUT_VARIABLE ldd_output
		ERROR_VARIABLE ldd_error
		RESULT_VARIABLE ldd_result
	)

	if(ldd_result)
		message(WARNING "SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY: Failed to run ldd on '${target_file}': ${ldd_error}")
		return()
	endif()

	# Check if output contains boost libraries
	string(TOLOWER "${ldd_output}" ldd_output_lower)
	string(FIND "${ldd_output_lower}" "libboost" boost_found)

	if(boost_found EQUAL -1)
		message(STATUS "SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY: WARNING - No Boost libraries found in dependencies of '${target_file}'")
		message(STATUS "ldd output:")
		message(STATUS "${ldd_output}")
	else()
		# Extract boost library lines
		string(REGEX MATCHALL "libboost[^\n]*" boost_libs "${ldd_output}")
		message(STATUS "SMO_VERIFY_BOOST_DYNAMIC_DEPENDENCY: SUCCESS - Boost libraries found in '${target_file}':")
		foreach(boost_lib ${boost_libs})
			string(STRIP "${boost_lib}" boost_lib_stripped)
			message(STATUS "  ${boost_lib_stripped}")
		endforeach()
	endif()
endfunction()

# Script mode: if VERIFY_FILE is defined, run the verification
if(VERIFY_FILE)
	_verify_boost_dynamic_dependency("${VERIFY_FILE}")
endif()

