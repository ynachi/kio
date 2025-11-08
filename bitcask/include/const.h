//
// Created by Yao ACHI on 08/11/2025.
//

// Shared const for bitcask

#ifndef KIO_CONST_H
#define KIO_CONST_H
#include <cstdint>

namespace bitcask
{
    constexpr std::uint8_t FLAG_NONE = 0x00;
    constexpr uint8_t FLAG_TOMBSTONE = 0x01;
}

#endif  // KIO_CONST_H
