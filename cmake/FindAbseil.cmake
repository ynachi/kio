include(FetchContent)

message(STATUS "Setting up Google Abseil")

FetchContent_Declare(
        abseil-cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20260107.1
        EXCLUDE_FROM_ALL                  # Don't build Abseil's tests/docs by default
)

# Configure Abseil build options BEFORE MakeAvailable
set(ABSL_BUILD_TESTING OFF CACHE BOOL "Build Abseil tests" FORCE)
set(ABSL_BUILD_DOCS OFF CACHE BOOL "Build Abseil docs" FORCE)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "Use system googletest if needed" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "Don't install Abseil" FORCE)  # Optional

FetchContent_MakeAvailable(abseil-cpp)
