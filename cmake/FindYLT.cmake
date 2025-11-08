include(FetchContent)

set(INSTALL_THIRDPARTY OFF CACHE BOOL "" FORCE)
set(INSTALL_STANDALONE OFF CACHE BOOL "" FORCE)
set(INSTALL_INDEPENDENT_THIRDPARTY OFF CACHE BOOL "" FORCE)
set(INSTALL_INDEPENDENT_STANDALONE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        yalantinglibs
        GIT_REPOSITORY https://github.com/alibaba/yalantinglibs.git
        GIT_TAG main
        GIT_SHALLOW 1
)

FetchContent_MakeAvailable(yalantinglibs)