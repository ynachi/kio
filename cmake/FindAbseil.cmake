include(FetchContent)

message(STATUS "Setting up Google Abseil")

FetchContent_Declare(
        abseil-cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20260107.0
)
FetchContent_MakeAvailable(abseil-cpp)