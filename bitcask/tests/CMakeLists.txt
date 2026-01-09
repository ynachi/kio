# ============================================================================
# Bitcask Test Suite
# ============================================================================

# Helper function for test executables
function(add_bitcask_test test_name source_file)
    add_executable(${test_name} ${source_file})

    target_link_libraries(${test_name} PRIVATE
            kio_bitcask
            kio_lib
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(${test_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/bitcask/include
            ${CMAKE_SOURCE_DIR}/core/include
    )

    kio_target_compile_options(${test_name})

    add_test(NAME ${test_name} COMMAND ${test_name})
    gtest_discover_tests(${test_name})
endfunction()

# ============================================================================
# Unit Tests
# ============================================================================

add_bitcask_test(entry_test
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/entry_test.cpp
)
set_tests_properties(entry_test PROPERTIES
        LABELS "unit"
        TIMEOUT 30
)

add_bitcask_test(config_test
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/config_test.cpp
)
set_tests_properties(config_test PROPERTIES
        LABELS "unit"
        TIMEOUT 30
)

add_bitcask_test(data_file_test
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/data_file_test.cpp
)
set_tests_properties(data_file_test PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)

# ============================================================================
# Integration Tests
# ============================================================================

add_bitcask_test(partition_test
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/partition_test.cpp
)
set_tests_properties(partition_test PROPERTIES
        LABELS "integration"
        TIMEOUT 120
)

add_bitcask_test(compactor_test
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/compactor_test.cpp
)
set_tests_properties(compactor_test PROPERTIES
        LABELS "integration"
        TIMEOUT 120
)

add_bitcask_test(bitkv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/bitkv_test.cpp
)
set_tests_properties(bitkv_test PROPERTIES
        LABELS "integration"
        TIMEOUT 300
)

# ============================================================================
# Test Targets
# ============================================================================

# Run all unit tests (fast)
add_custom_target(test_unit
        COMMAND ${CMAKE_CTEST_COMMAND} -L unit --output-on-failure
        DEPENDS entry_test config_test data_file_test
        COMMENT "Running unit tests"
)

# Run all integration tests
add_custom_target(test_integration
        COMMAND ${CMAKE_CTEST_COMMAND} -L integration --output-on-failure
        DEPENDS partition_test bitkv_test
        COMMENT "Running integration tests"
)

# Run all tests
add_custom_target(test_all
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS entry_test config_test data_file_test partition_test bitkv_test
        COMMENT "Running all tests"
)

# Quick smoke test (fastest subset)
add_custom_target(test_smoke
        COMMAND ${CMAKE_CTEST_COMMAND} -R "entry_test|config_test" --output-on-failure
        DEPENDS entry_test config_test
        COMMENT "Running smoke tests"
)

# ============================================================================
# Usage
# ============================================================================
#
# Build:
#   cmake --build . --target entry_test
#   cmake --build . --target test_all
#
# Run:
#   ctest                          # All tests
#   ctest -L unit                  # Unit tests only
#   ctest -L integration           # Integration tests only
#   ctest -R partition             # Tests matching "partition"
#   ctest --output-on-failure      # Show output on failure
#
# Make targets:
#   make test_smoke                # Fast smoke test
#   make test_unit                 # All unit tests
#   make test_integration          # All integration tests
#   make test_all                  # Everything
#
# GTest filters:
#   ./partition_test --gtest_filter=PartitionTest.Recovery*
#   ./bitkv_test --gtest_filter=*Concurrent*
#
# ============================================================================