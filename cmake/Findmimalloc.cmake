# kio/cmake/Findmimalloc.cmake
include(FetchContent)

message(STATUS "Setting up mimalloc")

FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v3.3.2
)

# Options for mimalloc
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(mimalloc)
