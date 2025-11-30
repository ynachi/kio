
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

add_subdirectory(tests)
