pkg_check_modules(LIBURING REQUIRED liburing)

set(CORO_HEADER_FILES include/coro.h
        include/io_pool.h)
set(CORO_SRC_FILES src/coro.cpp
        src/io_pool.cpp)

add_library(coro_lib ${CORO_HEADER_FILES} ${CORO_SRC_FILES})
target_link_libraries(coro_lib PUBLIC ${LIBURING_LIBRARIES} spdlog::spdlog)
