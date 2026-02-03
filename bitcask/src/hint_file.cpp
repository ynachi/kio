//
// Created by Yao ACHI on 14/11/2025.
//

#include "bitcask/include/hint_file.h"
using namespace kio;
using namespace kio::io;

namespace bitcask
{
HintFile::HintFile(const int fd, const uint64_t file_id, Worker& io_worker, BitcaskConfig& config)
    : file_id_(file_id), handle_(fd), io_worker_(io_worker), config_(config)
{
    if (fd < 0)
    {
        // this is a bug from the developer, so just throw
        throw std::invalid_argument("fd cannot be negative");
    }
}
}  // namespace bitcask
