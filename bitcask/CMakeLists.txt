
SET(BITCASK_H
        include/common.h
        include/keydir.h
        include/entry.h
        include/data_file.h
        include/config.h
)

SET(BITCASK_SRC
        src/bitcask.cpp
        src/keydir.cpp
        src/entry.cpp
        src/data_file.cpp
)

add_library(kio_bitcask ${BITCASK_H} ${BITCASK_SRC})
target_link_libraries(kio_bitcask yalantinglibs::yalantinglibs crc32c)

add_executable(bitcast_demo main.cpp)
target_link_libraries(bitcast_demo PRIVATE yalantinglibs::yalantinglibs crc32c)


## tests
add_executable(bitcask_test tests/keydir_test.cpp tests/entry_test.cpp tests/datafile_test.cpp)

target_link_libraries(bitcask_test
        PRIVATE
        kio_bitcask
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(bitcask_test)