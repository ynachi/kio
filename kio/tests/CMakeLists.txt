# ============================================================================
# Test Helper
# ============================================================================

# Ensure CMAKE_RUNTIME_OUTPUT_DIRECTORY is set to avoid CMP0174 warning
if (NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY OR CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL "")
    set(TEST_WORKING_DIR ${CMAKE_CURRENT_BINARY_DIR})
else ()
    set(TEST_WORKING_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif ()

# Helper function to create test targets
function(add_kio_test test_name source_file)
    add_executable(${test_name} ${source_file})

    # Link required libraries
    target_link_libraries(${test_name} PRIVATE
            kio_lib
            GTest::gtest
            GTest::gtest_main
    )

    # Compiler warnings & flags
    target_compile_options(${test_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-gnu-statement-expression
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O3>
    )

    # Register with CTest
    add_test(
            NAME ${test_name}
            COMMAND ${test_name}
    )

    # Test metadata
    set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 600              # 10 minutes
            LABELS "integration"
    )

    # GTest automatic test discovery
    gtest_discover_tests(${test_name}
            WORKING_DIRECTORY ${TEST_WORKING_DIR}
            PROPERTIES
            TIMEOUT 600
            LABELS "integration"
    )
endfunction()

# ============================================================================
# Define unit Tests
# ============================================================================
add_kio_test(basic_resp_parser
        ${CMAKE_CURRENT_SOURCE_DIR}/resp/parser_test.cpp
)