/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "../common/SegmentMemoryCache.h"

#include <gtest/gtest.h>

TEST(SegmentMemoryCache, ReusesSegmentsAndEvictsLeastRecentlyUsed)
{
  ADP::SegmentMemoryCache cache{6};
  ADP::SegmentMemoryCache::Key first{"segment-1", {}, 1, 100};
  ADP::SegmentMemoryCache::Key second{"segment-2", {}, 2, 200};
  ADP::SegmentMemoryCache::Key third{"segment-3", {}, 3, 300};
  std::vector<uint8_t> bytes;

  cache.Put(first, {1, 2, 3});
  cache.Put(second, {4, 5, 6});
  ASSERT_TRUE(cache.Get(first, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));

  cache.Put(third, {7, 8, 9});
  EXPECT_FALSE(cache.Get(second, bytes));
  EXPECT_TRUE(cache.Get(first, bytes));
  EXPECT_TRUE(cache.Get(third, bytes));
}

TEST(SegmentMemoryCache, SeparatesRangesAndSegmentTimestamps)
{
  ADP::SegmentMemoryCache cache{8};
  ADP::SegmentMemoryCache::Key key{"segment", {{"Range", "bytes=0-3"}}, 1, 100};
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
