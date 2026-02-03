//
// Created by Yao ACHI on 09/01/2026.
//

#include <gtest/gtest.h>
#include <fcntl.h>

#include "bitcask/include/config.h"

using namespace bitcask;

class ConfigTest : public ::testing::Test
{
protected:
    BitcaskConfig config_;

    void SetUp() override
    {
        config_.directory = "/tmp/test_db";
        config_.max_file_size = 100 * 1024 * 1024;
        config_.fragmentation_threshold = 0.5;
        config_.write_flags = O_CREAT | O_WRONLY;
    }
};

// ============================================================================
// O_APPEND Validation (Critical - Prevents Data Corruption)
// ============================================================================

TEST_F(ConfigTest, RejectsOAppendInWriteFlags)
{
    // O_APPEND breaks pwrite() offset semantics on Linux
    // This would cause silent data corruption
    config_.write_flags = O_CREAT | O_WRONLY | O_APPEND;

    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, AcceptsValidWriteFlags)
{
    config_.write_flags = O_CREAT | O_WRONLY;
    EXPECT_NO_THROW(config_.Validate());

    config_.write_flags = O_CREAT | O_RDWR;
    EXPECT_NO_THROW(config_.Validate());

    config_.write_flags = O_CREAT | O_WRONLY | O_TRUNC;
    EXPECT_NO_THROW(config_.Validate());
}

// ============================================================================
// Directory Validation
// ============================================================================

TEST_F(ConfigTest, RejectsEmptyDirectory)
{
    config_.directory = "";
    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, AcceptsNonEmptyDirectory)
{
    config_.directory = "/some/path";
    EXPECT_NO_THROW(config_.Validate());
}

// ============================================================================
// Fragmentation Threshold Validation
// ============================================================================

TEST_F(ConfigTest, RejectsZeroFragmentationThreshold)
{
    config_.fragmentation_threshold = 0.0;
    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsNegativeFragmentationThreshold)
{
    config_.fragmentation_threshold = -0.1;
    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsFragmentationThresholdAboveOne)
{
    config_.fragmentation_threshold = 1.1;
    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, AcceptsValidFragmentationThreshold)
{
    config_.fragmentation_threshold = 0.5;
    EXPECT_NO_THROW(config_.Validate());

    config_.fragmentation_threshold = 1.0;
    EXPECT_NO_THROW(config_.Validate());

    config_.fragmentation_threshold = 0.01;
    EXPECT_NO_THROW(config_.Validate());
}

// ============================================================================
// Max File Size Validation
// ============================================================================

TEST_F(ConfigTest, RejectsZeroMaxFileSize)
{
    config_.max_file_size = 0;
    EXPECT_THROW(config_.Validate(), std::invalid_argument);
}

TEST_F(ConfigTest, AcceptsNonZeroMaxFileSize)
{
    config_.max_file_size = 1;
    EXPECT_NO_THROW(config_.Validate());

    config_.max_file_size = 1024 * 1024 * 1024;  // 1GB
    EXPECT_NO_THROW(config_.Validate());
}

// ============================================================================
// Default Values
// ============================================================================

TEST_F(ConfigTest, DefaultsAreValid)
{
    BitcaskConfig defaults;
    defaults.directory = "/tmp/test";  // Only required field

    EXPECT_NO_THROW(defaults.Validate());
}

TEST_F(ConfigTest, DefaultWriteFlagsDoNotIncludeOAppend)
{
    const BitcaskConfig defaults;
    EXPECT_EQ(defaults.write_flags & O_APPEND, 0)
        << "Default write_flags should not include O_APPEND";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}