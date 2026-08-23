add_library(spinscale_test_support STATIC
	support/threadHarness.cpp
)

target_include_directories(spinscale_test_support PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/tests/fixtures
)

target_link_libraries(spinscale_test_support PUBLIC
	spinscale
	spinscale_probe_support
	gtest
)

function(add_spinscale_gtest target)
	add_executable(${target} ${ARGN})
	target_link_libraries(${target} PRIVATE
		spinscale_test_support
		gtest_main
	)
	add_dependencies(${target} gtest_main)
	add_test(NAME ${target} COMMAND ${target})
endfunction()

add_spinscale_gtest(spinscale_env_kv_store_tests
	env_kv_store_test.cpp
)

add_spinscale_gtest(qutex_tests
	cps/qutex_tests.cpp
)

add_spinscale_gtest(nonViralTaskNursery_tests
	co/nonViralTaskNursery_tests.cpp
)

add_spinscale_gtest(co_viral_non_posting_tests
	co/viral_non_posting_tests.cpp
)

add_spinscale_gtest(co_posting_cross_thread_tests
	co/posting_cross_thread_tests.cpp
)

add_spinscale_gtest(co_group_edge_tests
	co/group_edge_tests.cpp
)

add_spinscale_gtest(co_group_timer_tests
	co/group_timer_tests.cpp
)

add_spinscale_gtest(co_component_continuation_tests
	co/component_continuation_tests.cpp
)

add_spinscale_gtest(threadHarnessLifecycle_tests
	support/threadHarnessLifecycle_tests.cpp
)
