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
    constexpr size_t MIN_ON_DISK_SIZE = 12;  // at least CRC + PAYLOAD SIZE

    /**
     * @brief Reads a Little-Endian integer from a raw buffer.
     *
     * This function safely reads a value of type T (e.g., uint32_t, uint64_t)
     * from a byte buffer that is known to be in Little-Endian format.
     * It correctly handles the byte order, swapping if the host machine
     * is Big-Endian.
     *
     * @tparam T The integer type to read (e.g., uint32_t, uint64_t).
     * @param buffer A pointer to the start of the byte buffer.
     * @return The integer value in the host machine's native format.
     */
    template<typename T>
    constexpr T read_le(const char* buffer)
    {
        // 1. Safely copy the bytes from the buffer into the integer.
        // This avoids strict-aliasing violations from reinterpret_cast.
        T val;
        std::memcpy(&val, buffer, sizeof(T));

        // 2. At compile-time, check if our machine is Big-Endian.
        if constexpr (std::endian::native == std::endian::big)
        {
            // 3. If so, swap the bytes to convert from LE (file)
            //    to BE (host).
            return std::byteswap(val);
        }
        else
        {
            // 4. If we are on a Little-Endian machine, the bytes
            //    are already correct. Do nothing.
            return val;
        }
    }
}  // namespace bitcask

#endif  // KIO_CONST_H
