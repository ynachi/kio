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
