#include "bitcask/segment.hpp"

#include "uring/core/io.h"

#include <filesystem>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

class SegmentManager : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto io = URing::IO(0, nullptr,
                            {
        },
                            {
                                {.size = 1024, .count = 256},
                                {.size = 4096, .count = 16},
                            });
        io_ = std::make_unique<URing::IO>(std::move(io));

        fs::path temp_base = fs::temp_directory_path();
        test_dir_ = temp_base / "kio_test_dir_12345";
        fs::create_directories(test_dir_);

        bitcask::BitcaskConfig cfg;
        cfg.directory = test_dir_;
        cfg.max_segment_size = 1024 * 1024;  // 1MB

        manager_ = std::make_unique<bitcask::SegmentManager>(0, cfg, 0, 0);
    }

    void TearDown() override { std::filesystem::remove_all(test_dir_); }

    std::unique_ptr<URing::IO> io_;
    std::filesystem::path test_dir_;
    std::unique_ptr<bitcask::SegmentManager> manager_;
};