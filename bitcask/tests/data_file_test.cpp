//
// Created by Yao ACHI on 08/01/2026.
//

// Test 1: Concurrent coroutine writes don't overlap
TEST(DataFile, ConcurrentCoroutineWritesNoOverlap)
{
    // Launch 100 Put() coroutines simultaneously on same partition
    // Verify all entries are readable and distinct
    // Verify file has no overlapping regions
}

// Test 2: O_APPEND rejected
TEST(DataFile, RejectsOAppend)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    EXPECT_THROW(DataFile(fd, ...), std::invalid_argument);
    close(fd);
}

// Test 3: Config validation
TEST(Config, RejectsOAppendInWriteFlags)
{
    BitcaskConfig config;
    config.write_flags = O_CREAT | O_WRONLY | O_APPEND;
    EXPECT_THROW(config.Validate(), std::invalid_argument);
}