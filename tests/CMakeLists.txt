# Test executables
add_executable(sync_wait_test sync_wait_test.cpp)

target_link_libraries(sync_wait_test
        PRIVATE
        core_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(sync_wait_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Discover and register tests
gtest_discover_tests(sync_wait_test)

add_custom_target(run_tests
        COMMAND sync_wait_test --gtest_color=yes
        DEPENDS sync_wait_test
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running AIO tests..."
)