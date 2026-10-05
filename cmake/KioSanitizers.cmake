function(kio_configure_sanitizers)
    add_library(kio-sanitizers INTERFACE)
    add_library(kio::sanitizers ALIAS kio-sanitizers)

    if (KIO_USE_MIMALLOC AND NOT KIO_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "mimalloc replaces the allocator the sanitizers need; use one or the other")
    endif ()

    # Sanitizers apply to every target, fetched dependencies included, so one
    # build tree is instrumented consistently. mimalloc must stay off with them.
    # ---------------------------------------------------------------------------
    if (KIO_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=address,undefined)
    elseif (KIO_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    elseif (NOT KIO_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "KIO_SANITIZER must be none, address or thread")
    endif ()
endfunction()
