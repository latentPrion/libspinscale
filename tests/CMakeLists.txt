add_executable(spinscale_env_kv_store_tests
	env_kv_store_test.cpp
)

target_link_libraries(spinscale_env_kv_store_tests PRIVATE
	spinscale
	gtest_main
)

add_dependencies(spinscale_env_kv_store_tests gtest_main)
add_test(NAME spinscale_env_kv_store_tests
	COMMAND spinscale_env_kv_store_tests)

add_executable(qutex_tests
	cps/qutex_tests.cpp
)

target_link_libraries(qutex_tests PRIVATE
	spinscale
	gtest_main
)

add_dependencies(qutex_tests gtest_main)
add_test(NAME qutex_tests COMMAND qutex_tests)

add_executable(nonViralTaskNursery_tests
	co/nonViralTaskNursery_tests.cpp
)

target_link_libraries(nonViralTaskNursery_tests PRIVATE
	spinscale
	gtest_main
)

add_dependencies(nonViralTaskNursery_tests gtest_main)
add_test(NAME nonViralTaskNursery_tests
	COMMAND nonViralTaskNursery_tests)
