set(PHOTON_ENABLE_URING ON CACHE INTERNAL "Enable iouring")
set(PHOTON_CXX_STANDARD 14 CACHE INTERNAL "C++ standard")

add_compile_options(-Wno-everything)

# Use my own fork, with io_uring dependency fixes
include(FetchContent)
FetchContent_Declare(
        photon
        GIT_REPOSITORY https://github.com/ynachi/PhotonLibOS.git
        GIT_TAG main
)
FetchContent_MakeAvailable(photon)

# Specifically for Photon targets, suppress all warnings
if (TARGET photon_static)
    target_compile_options(photon_static PRIVATE
            -w  # Disable all warnings for GCC/Clang
    )
endif ()
if (TARGET photon_shared)
    target_compile_options(photon_shared PRIVATE
            -w
    )
endif ()