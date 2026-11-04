function(kio_configure_warnings)
    add_library(kio_warnings INTERFACE)
    add_library(kio::warnings ALIAS kio_warnings)

    target_compile_options(
            kio_warnings
            INTERFACE
            -Wall
            -Wextra
            -Wconversion
            -Wpedantic
            -Wshadow
            -Wsign-conversion
            -Werror)
endfunction()
