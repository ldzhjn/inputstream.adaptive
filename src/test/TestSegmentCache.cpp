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

namespace
{
void AddTimedSegment(SegmentCache& cache,
                     uint64_t number,
                     uint64_t begin,
                     uint64_t end,
                     const std::string& track)
{
  SegmentCache::Key key{std::to_string(number), {}, number, begin};
  key.cacheTrackId = track;
  key.cacheStartUs = begin;
  key.cacheEndUs = end;
  PLAYLIST::CSegment segment;
  segment.m_number = number;
  segment.startPTS_ = begin;
  segment.m_endPts = end;
  cache.Put(std::move(key), {1}, {}, segment);
}
} // namespace

TEST(SegmentCache, ReportsOnlySelectedTracksIntersectionAndEvictedGaps)
{
  for (const auto mode : {SegmentCache::Mode::MEMORY, SegmentCache::Mode::DISK})
  {
    const auto root = std::filesystem::temp_directory_path() /
                      ("isa-cache-overlap-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
      SegmentCache cache{mode, 5, root};
      ASSERT_TRUE(cache.IsAvailable());
      AddTimedSegment(cache, 1, 0, 1000000, "video-2160p");
      AddTimedSegment(cache, 2, 1000000, 2000000, "video-2160p");
      AddTimedSegment(cache, 3, 500000, 1500000, "audio-en");
      AddTimedSegment(cache, 4, 0, 2000000, "audio-ja");
      AddTimedSegment(cache, 5, 0, 2000000, "video-1440p");
      EXPECT_EQ(cache.GetCachedRanges({"video-2160p", "audio-en"}),
                (std::vector<std::pair<uint64_t, uint64_t>>{{500000, 1500000}}));
      EXPECT_TRUE(cache.GetCachedRanges({"video-2160p", "audio-zh"}).empty());
      EXPECT_EQ(cache.GetCachedRanges({"video-2160p", "audio-ja"}),
                (std::vector<std::pair<uint64_t, uint64_t>>{{0, 2000000}}));

      // Evict only the earliest 2160p segment. Lower quality cached bytes must
      // not fill its gap for the current 2160p selection.
      AddTimedSegment(cache, 6, 3000000, 4000000, "video-2160p");
      EXPECT_EQ(cache.GetCachedRanges({"video-2160p"}),
                (std::vector<std::pair<uint64_t, uint64_t>>{{1000000, 2000000},
                                                          {3000000, 4000000}}));
      EXPECT_EQ(cache.GetCachedRanges({"video-2160p", "audio-en"}),
                (std::vector<std::pair<uint64_t, uint64_t>>{{1000000, 1500000}}));
      // English audio was evicted; Japanese audio must not stand in for it.
      AddTimedSegment(cache, 7, 4000000, 5000000, "video-2160p");
      AddTimedSegment(cache, 8, 5000000, 6000000, "video-2160p");
      EXPECT_TRUE(cache.GetCachedRanges({"video-2160p", "audio-en"}).empty());
    }
    EXPECT_TRUE(mode == SegmentCache::Mode::MEMORY || std::filesystem::is_empty(root));
    if (mode == SegmentCache::Mode::DISK)
      std::filesystem::remove(root);
  }
}

TEST(SegmentCache, RejectsDifferentByteRangeEvenWhenMediaIdentityMatches)
{
  SegmentCache cache{SegmentCache::Mode::MEMORY, 32};
  SegmentCache::Key first{"old-signature", {{"Range", "bytes=10-19"}}, 1, 0};
  first.cacheTrackId = "video";
  PLAYLIST::CSegment segment;
  segment.startPTS_ = 0;
  segment.m_endPts = 1000;
  segment.range_begin_ = 10;
  segment.range_end_ = 19;
  cache.Put(first, {1, 2}, {}, segment);
  auto refreshed = first;
  refreshed.url = "new-signature";
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(cache.Get(refreshed, bytes));
  refreshed.headers["Range"] = "bytes=20-29";
  EXPECT_FALSE(cache.Get(refreshed, bytes));
  refreshed.headers["Range"] = "bytes=10-19";
  refreshed.cacheTrackId = "other-audio";
  EXPECT_FALSE(cache.Get(refreshed, bytes));
}

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

  auto differentKey = key;
  differentKey.keyUrl = "new-key";
  EXPECT_FALSE(cache.Get(differentKey, bytes));

  auto differentPeriod = key;
  differentPeriod.periodSequence = 1;
  EXPECT_FALSE(cache.Get(differentPeriod, bytes));

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
  std::vector<SegmentCache::Chunk> chunks;

  {
    SegmentCache cache{SegmentCache::Mode::DISK, 6, root};
    ASSERT_TRUE(cache.IsAvailable());
    cache.Put(first, {1, 2, 3}, {{1, false}, {2, true}});
    cache.Put(second, {4, 5, 6});
    ASSERT_TRUE(cache.Get(first, bytes, &chunks));
    EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(chunks, (std::vector<SegmentCache::Chunk>{{1, false}, {2, true}}));
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
    EXPECT_TRUE(std::filesystem::is_empty(sessionDirectory.path()));
  }

  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove(root);
}

TEST(SegmentCache, RejectsNonLocalDiskPath)
{
  SegmentCache cache{SegmentCache::Mode::DISK, 6, "smb://server/share"};
  EXPECT_FALSE(cache.IsAvailable());
}

TEST(SegmentCache, InitializationDataIsReplayableButNotTimelineMedia)
{
  SegmentCache cache{SegmentCache::Mode::MEMORY, 6};
  SegmentCache::Key key{"init.m4i", {}, 0, 0};
  key.periodId = "period";
  key.adaptationId = "video";
  key.representationId = "720p";
  PLAYLIST::CSegment init;
  init.SetIsInitialization(true);
  init.AESKeyInfo() = CAesKeyInfo{{1, 2, 3}, {}, "key-uri"};
  cache.Put(key, {4, 5}, {}, init);

  std::vector<uint8_t> bytes;
  std::optional<PLAYLIST::CSegment> cachedSegment;
  ASSERT_TRUE(cache.Get(key, bytes, nullptr, &cachedSegment));
  ASSERT_TRUE(cachedSegment);
  EXPECT_TRUE(cachedSegment->IsInitialization());
  EXPECT_EQ(cachedSegment->AESKeyInfo()->key, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_TRUE(cache.GetSegments(0, "period", 0, "video", "720p").empty());
  EXPECT_FALSE(cache.HasPeriod(0, "period", 0));
}

TEST(SegmentCache, RetainsOnlyDownloadedTimelineAcrossUrlRefreshAndEviction)
{
  for (const auto mode : {SegmentCache::Mode::MEMORY, SegmentCache::Mode::DISK})
  {
    const auto root = std::filesystem::temp_directory_path() /
                      ("isa-cache-history-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
      SegmentCache cache{mode, 6, root};
      ASSERT_TRUE(cache.IsAvailable());

      SegmentCache::Key first{"old-token/1", {}, 1, 100};
      first.periodId = "period";
      first.adaptationId = "video";
      first.representationId = "720p";
      PLAYLIST::CSegment firstSegment;
      firstSegment.m_number = 1;
      firstSegment.startPTS_ = 100;
      firstSegment.m_endPts = 106;
      cache.Put(first, {1, 2, 3}, {}, firstSegment);

      auto second = first;
      second.url = "old-token/2";
      second.number = 2;
      second.startPts = 106;
      auto secondSegment = firstSegment;
      secondSegment.m_number = 2;
      secondSegment.startPTS_ = 106;
      secondSegment.m_endPts = 112;
      cache.Put(second, {4, 5, 6}, {}, secondSegment);

      auto segments = cache.GetSegments(0, "period", 0, "video", "720p");
      ASSERT_EQ(segments.size(), 2U);
      EXPECT_EQ(segments[0].m_number, 1U);
      EXPECT_EQ(segments[1].m_number, 2U);
      EXPECT_TRUE(cache.HasPeriod(0, "period", 0));

      std::vector<uint8_t> bytes;
      auto refreshed = first;
      refreshed.url = "new-token/1";
      EXPECT_TRUE(cache.Get(refreshed, bytes));
      EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));

      auto third = second;
      third.url = "new-token/3";
      third.number = 3;
      third.startPts = 112;
      auto thirdSegment = secondSegment;
      thirdSegment.m_number = 3;
      thirdSegment.startPTS_ = 112;
      thirdSegment.m_endPts = 118;
      cache.Put(third, {7, 8, 9}, {}, thirdSegment);
      segments = cache.GetSegments(0, "period", 0, "video", "720p");
      ASSERT_EQ(segments.size(), 2U);
      EXPECT_EQ(segments[0].m_number, 1U);
      EXPECT_EQ(segments[1].m_number, 3U);
    }
    if (mode == SegmentCache::Mode::DISK)
      std::filesystem::remove(root);
  }
}
