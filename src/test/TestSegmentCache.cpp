/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "../common/SegmentCache.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using ADP::SegmentCache;

TEST(SegmentCache, SharesOneMemoryLimitAcrossStreams)
{
  SegmentCache cache{SegmentCache::Mode::MEMORY, 6};
  SegmentCache::Key video{"video-1", {}, 1, 100};
  SegmentCache::Key audio{"audio-1", {}, 1, 100};
  SegmentCache::Key nextVideo{"video-2", {}, 2, 200};
  std::vector<uint8_t> bytes;

  cache.Put(video, {1, 2, 3});
  cache.Put(audio, {4, 5, 6});
  ASSERT_TRUE(cache.Get(video, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));

  cache.Put(nextVideo, {7, 8, 9});
  EXPECT_FALSE(cache.Get(audio, bytes));
  EXPECT_TRUE(cache.Get(video, bytes));
  EXPECT_TRUE(cache.Get(nextVideo, bytes));
}

TEST(SegmentCache, SeparatesRangesAndSegmentTimestamps)
{
  SegmentCache cache{SegmentCache::Mode::MEMORY, 8};
  SegmentCache::Key key{"segment", {{"Range", "bytes=0-3"}}, 1, 100};
  std::vector<uint8_t> bytes;
  cache.Put(key, {1, 2, 3, 4});

  auto differentRange = key;
  differentRange.headers["Range"] = "bytes=4-7";
  EXPECT_FALSE(cache.Get(differentRange, bytes));

  auto differentTime = key;
  differentTime.startPts = 200;
  EXPECT_FALSE(cache.Get(differentTime, bytes));

  EXPECT_TRUE(cache.Get(key, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3, 4}));
  cache.Put(differentTime, std::vector<uint8_t>(9, 0));
  EXPECT_FALSE(cache.Get(differentTime, bytes));
}

TEST(SegmentCache, DiskEntriesEvictAndDisappearOnClose)
{
  const auto root = std::filesystem::temp_directory_path() /
                    ("isa-segment-cache-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  SegmentCache::Key first{"video-1", {}, 1, 100};
  SegmentCache::Key second{"audio-1", {}, 1, 100};
  SegmentCache::Key third{"video-2", {}, 2, 200};
  std::vector<uint8_t> bytes;

  {
    SegmentCache cache{SegmentCache::Mode::DISK, 6, root};
    ASSERT_TRUE(cache.IsAvailable());
    cache.Put(first, {1, 2, 3});
    cache.Put(second, {4, 5, 6});
    ASSERT_TRUE(cache.Get(first, bytes));
    EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));
    cache.Put(third, {7, 8, 9});
    EXPECT_FALSE(cache.Get(second, bytes));
    ASSERT_TRUE(cache.Get(third, bytes));
    EXPECT_EQ(bytes, (std::vector<uint8_t>{7, 8, 9}));
  }

  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove(root);
}

TEST(SegmentCache, RejectsTruncatedDiskEntry)
{
  const auto root = std::filesystem::temp_directory_path() /
                    ("isa-segment-cache-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  SegmentCache::Key key{"segment", {}, 1, 100};
  std::vector<uint8_t> bytes;

  {
    SegmentCache cache{SegmentCache::Mode::DISK, 6, root};
    ASSERT_TRUE(cache.IsAvailable());
    cache.Put(key, {1, 2, 3});
    const auto sessionDirectory = *std::filesystem::directory_iterator(root);
    const auto file = *std::filesystem::directory_iterator(sessionDirectory.path());
    std::ofstream(file.path(), std::ios::binary | std::ios::trunc).put(0);
    EXPECT_FALSE(cache.Get(key, bytes));
  }

  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove(root);
}

TEST(SegmentCache, RejectsNonLocalDiskPath)
{
  SegmentCache cache{SegmentCache::Mode::DISK, 6, "smb://server/share"};
  EXPECT_FALSE(cache.IsAvailable());
}
