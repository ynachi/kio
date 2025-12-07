include(FetchContent)

# gflags
FetchContent_Declare(
        gflags
        GIT_REPOSITORY https://github.com/gflags/gflags.git
        GIT_TAG        v2.3.0
)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(gflags)
