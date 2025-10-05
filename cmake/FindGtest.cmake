# Try to find GTest using the system package manager
find_package(GTest)

# If GTest is not found, download it using FetchContent
if(NOT GTest_FOUND)
    message(STATUS "GTest not found. Downloading from GitHub...")

    # Use FetchContent to download GTest
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        ${GTEST_GIT_TAG}
    )

    # Specify the extraction directory
    FetchContent_MakeAvailable(googletest)

    # Add GTest include directory to your project
    include(GoogleTest)
endif()