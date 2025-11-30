# ============================================================================
# Test Executables
# ============================================================================

# Helper function to create test targets
function(add_bitkv_test test_name source_file)
    add_executable(${test_name} ${source_file})

    target_link_libraries(${test_name} PRIVATE
            kio_bitcask
            kio_lib
            GTest::gtest
            GTest::gtest_main
    )

    # Include directories (adjust paths as needed)
    target_include_directories(${test_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/bitcask/include
            ${CMAKE_SOURCE_DIR}/core/include
    )

    # Compiler flags
    target_compile_options(${test_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-gnu-statement-expression
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O3>
    )

    # Add to CTest
    add_test(NAME ${test_name} COMMAND ${test_name})

    # Set test properties
    set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 600  # 10 minutes timeout
            LABELS "integration"
    )

    # gtest discovery
    gtest_discover_tests(${test_name})
endfunction()

# ============================================================================
# Define unit Tests
# ============================================================================
add_bitkv_test(entry_encode_decode_test
        ${CMAKE_CURRENT_SOURCE_DIR}/entry_codec_test.cpp
)

add_bitkv_test(file_operations_test
        ${CMAKE_CURRENT_SOURCE_DIR}/file_operations_test.cpp
)

add_bitkv_test(partition_test
        ${CMAKE_CURRENT_SOURCE_DIR}/partition_test.cpp
)

add_bitkv_test(compactor_test
        ${CMAKE_CURRENT_SOURCE_DIR}/compactor_test.cpp
)

add_bitkv_test(partition_integration_test
        ${CMAKE_CURRENT_SOURCE_DIR}/partition_integration_test.cpp
)

# ============================================================================
# Define Integration Tests
# ============================================================================

# Core integration tests
add_bitkv_test(bitkv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/bitkv_test.cpp
)

# Advanced integration tests (performance & concurrency)
add_bitkv_test(bitkv_advanced_test
        ${CMAKE_CURRENT_SOURCE_DIR}/bitkv_advanced_test.cpp
)

# Error handling tests
#add_bitkv_test(bitkv_error_test
#        ${CMAKE_CURRENT_SOURCE_DIR}/bitkv_error_test.cpp
#)

# ============================================================================
# Test Groups (Optional)
# ============================================================================

# Quick tests (for rapid development)
add_custom_target(test_quick
        COMMAND ${CMAKE_CTEST_COMMAND} -L integration -R "BitKVTest\\.BasicPutGet|BitKVTest\\.BasicGet"
        DEPENDS bitkv_test
        COMMENT "Running quick smoke tests"
)

# All integration tests
#add_custom_target(test_integration
#        COMMAND ${CMAKE_CTEST_COMMAND} -L integration
#        DEPENDS bitkv_test bitkv_advanced_test bitkv_error_test
#        COMMENT "Running all integration tests"
#)

# Performance tests only
#add_custom_target(test_perf
#        COMMAND ${CMAKE_CTEST_COMMAND} -R "Performance|HighVolume"
#        DEPENDS bitkv_advanced_test
#        COMMENT "Running performance tests"
#)

# ============================================================================
# Usage Examples
# ============================================================================

# Build tests:
#   cmake --build . --target bitkv_test
#   cmake --build . --target bitkv_advanced_test
#   cmake --build . --target bitkv_error_test
#
# Run all tests:
#   ctest
#   or: make test
#   or: ninja test
#
# Run specific test suite:
#   ./bitkv_test
#   ./bitkv_advanced_test
#   ./bitkv_error_test
#
# Run with CTest:
#   ctest -R bitkv          # Run all BitKV tests
#   ctest -R Basic          # Run tests matching "Basic"
#   ctest -V                # Verbose output
#   ctest --output-on-failure  # Show output only on failure
#
# Run custom targets:
#   make test_quick         # Quick smoke tests
#   make test_integration   # All integration tests
#   make test_perf         # Performance tests only
#
# Run with GTest filters:
#   ./bitkv_test --gtest_filter=BitKVTest.BasicPutGet
#   ./bitkv_test --gtest_filter=BitKVTest.Recovery*
#   ./bitkv_advanced_test --gtest_filter=*Concurrent*
#
# Generate test report:
#   ctest --output-on-failure --output-junit test_results.xml

# ============================================================================
# Test Coverage (Optional - requires gcov/lcov)
# ============================================================================

if (CMAKE_BUILD_TYPE STREQUAL "Coverage")
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)

    if (LCOV AND GENHTML)
        # Add coverage flags
        target_compile_options(bitkv_test PRIVATE --coverage)
        target_link_options(bitkv_test PRIVATE --coverage)

        target_compile_options(bitkv_advanced_test PRIVATE --coverage)
        target_link_options(bitkv_advanced_test PRIVATE --coverage)

        target_compile_options(bitkv_error_test PRIVATE --coverage)
        target_link_options(bitkv_error_test PRIVATE --coverage)

        # Coverage target
        add_custom_target(coverage
                COMMAND ${CMAKE_CTEST_COMMAND}
                COMMAND ${LCOV} --capture --directory . --output-file coverage.info
                COMMAND ${LCOV} --remove coverage.info '/usr/*' --output-file coverage.info
                COMMAND ${GENHTML} coverage.info --output-directory coverage_report
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating code coverage report"
        )
    endif ()
endif ()

