//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_HINT_FILE_H
#define KIO_HINT_FILE_H

namespace kio
{
    // Hint Entry, fully serialize/deserialize by struct_pack:
    // +----------------+--------+-----------+----------------+
    // | timestamp_ns(8)| entry_pos | total_sz  | key(string) |
    // +----------------+--------+-----------+----------------+
    // - timestamp_ns : uint64_t nanosecond timestamp
    // - entry_pos: position of the serialized data entry in the datafile
    // - total_sz: Total size of the serialized data entry in the datafile
    // - key:
}

#endif  // KIO_HINT_FILE_H
