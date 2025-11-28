
SET(BITCASK_H
        include/common.h
        include/entry.h
        include/data_file.h
        include/config.h
        include/hint_file.h
        include/compactor.h
        include/stats.h
        include/file_handle.h
        include/bitcask.h
        include/partition.h
        include/fd_cache.h
        include/file_id.h
)

SET(BITCASK_SRC
        src/common.cpp
        src/bitcask.cpp
        src/entry.cpp
        src/data_file.cpp
        src/hint_file.cpp
        src/compactor.cpp
        src/bitcask.cpp
        src/partition.cpp
        src/fd_cache.cpp
)

add_library(kio_bitcask ${BITCASK_H} ${BITCASK_SRC})
target_link_libraries(kio_bitcask yalantinglibs::yalantinglibs crc32c)

add_executable(bitcast_demo main.cpp)
target_link_libraries(bitcast_demo PRIVATE yalantinglibs::yalantinglibs crc32c)


#-----------------------------------
# TEST TARGETS
#------------------------------------

# entry operations
add_executable(entry_encode_decode_test
        tests/entry_codec_test.cpp
)

target_link_libraries(entry_encode_decode_test
        PRIVATE
        kio_bitcask
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

# file operations
add_executable(file_operations_test
        tests/file_operations_test.cpp
)

target_link_libraries(file_operations_test
        PRIVATE
        kio_bitcask
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

# partition tests
add_executable(partition_test
        tests/partition_test.cpp
)

target_link_libraries(partition_test
        PRIVATE
        kio_bitcask
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

# Compactor tests
add_executable(compactor_test
        tests/compactor_test.cpp
)

target_link_libraries(compactor_test
        PRIVATE
        kio_bitcask
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(entry_encode_decode_test)
gtest_discover_tests(file_operations_test)
gtest_discover_tests(partition_test)
gtest_discover_tests(compactor_test)
