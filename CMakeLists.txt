cmake_minimum_required(VERSION 3.30)
project(kio)

set(CMAKE_CXX_STANDARD 23)
set(GTEST_GIT_TAG v1.17.0)

# Enable Thread Sanitizer for debug builds
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
endif ()

#-----------------------------------------
# Dependencies
#----------------------------------------
find_package(PkgConfig REQUIRED)

# spdlog
include(cmake/FindSPlog.cmake)


#-----------------------------------------------
# Includes
#----------------------------------------------
add_subdirectory(coro)


#------------------------------
# Targets
#-----------------------------
add_executable(kio main.cpp)