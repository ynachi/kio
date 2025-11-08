
SET(BITCASK_H
        include/const.h
        include/keydir.h
)

SET(BITCASK_SRC
        src/bitcask.cpp
        src/keydir.cpp
)

add_library(kio_bitcask ${BITCASK_H} ${BITCASK_SRC})

add_executable(bitcast_demo main.cpp)
target_link_libraries(bitcast_demo PRIVATE yalantinglibs::yalantinglibs crc32c)


## tests
add_executable(bitcask_test tests/keydir_test.cpp)

target_link_libraries(bitcask_test
        PRIVATE
        kio_bitcask
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(bitcask_test)