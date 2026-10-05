/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "../SrvBroker.h"
#include "TestHelper.h"

#include <gtest/gtest.h>

namespace
{
class CacheReplayTree : public DASHTestTree
{
public:
  explicit CacheReplayTree(bool live) { m_isLive = live; }
};

class CacheReplayHLSTree : public HLSTestTree
{
public:
  explicit CacheReplayHLSTree(bool live) { m_isLive = live; }
};

// Execute downloads synchronously so the test exercises production cache and
// HTTP code without a concurrently running demuxer or downloader.
class CacheReplayStream : public adaptive::AdaptiveStream
{
public:
  CacheReplayStream(adaptive::AdaptiveTree* tree,
                    PLAYLIST::CAdaptationSet* adp,
                    PLAYLIST::CRepresentation* rep,
                    uint64_t originUs)
    : AdaptiveStream(tree, adp, rep)
  {
    absolutePTSOffset_ = originUs;
    thread_data_ = new THREADDATA;
    thread_data_->StartDownloads();
  }

  bool ReadSegment(const PLAYLIST::CSegment& segment,
                   std::vector<uint8_t>& bytes,
                   PLAYLIST::CRepresentation* rep = nullptr)
  {
    if (!rep)
      rep = current_rep_;
    ADP::SegmentBuffer buffer;
    buffer.segment = segment;
    buffer.rep = rep;
    DownloadInfo info;
    info.m_segmentBuffer = &buffer;
    if (!PrepareDownload(rep, segment, info) || !DownloadSegment(info))
      return false;
    bytes = buffer.ReadBuffer();
    return true;
  }
};

class SegmentCacheReplay : public ::testing::Test
{
protected:
  void SetUp() override
  {
    CSrvBroker::GetInstance()->Initialize();
    kodi::vfs::curlResponses.clear();
    kodi::vfs::curlOpenRequests.clear();
  }
  void TearDown() override
  {
    kodi::vfs::curlResponses.clear();
    kodi::vfs::curlOpenRequests.clear();
  }
};
} // namespace

TEST_F(SegmentCacheReplay, VODSegmentBaseReplaysByteRangesAndKeepsEarlierSeekCache)
{
  CacheReplayTree tree{false};
  auto period = PLAYLIST::CPeriod::MakeUniquePtr();
  period->SetId("movie");
  period->SetStart(0);
  period->SetIndex(0);
  period->SetTimescale(1000);
  period->SetTlDuration(4000);
  tree.m_currentPeriod = period.get();
  tree.m_periods.push_back(std::move(period));
  tree.RefreshChaptersSnapshot();
  auto cache = std::make_shared<ADP::SegmentCache>(ADP::SegmentCache::Mode::MEMORY, 64);
  tree.SetSegmentCacheForTest(cache);
  PLAYLIST::CAdaptationSet adp{tree.m_currentPeriod};
  adp.SetId("video");
  adp.SetStreamType(PLAYLIST::StreamType::VIDEO);
  PLAYLIST::CRepresentation rep{&adp};
  rep.SetId("2160p");
  rep.SetTimescale(1000);
  rep.SetBaseUrl("https://fixture.invalid/movie.mp4");
  rep.SetSegmentBase(PLAYLIST::CSegmentBase{});

  CacheReplayStream stream{&tree, &adp, &rep, 0};
  PLAYLIST::CSegment first;
  first.startPTS_ = 0;
  first.m_endPts = 1000;
  first.range_begin_ = 10;
  first.range_end_ = 19;
  PLAYLIST::CSegment later = first;
  later.startPTS_ = 3000;
  later.m_endPts = 4000;
  later.range_begin_ = 30;
  later.range_end_ = 39;
  rep.Timeline().Add(first);
  rep.Timeline().Add(later);
  kodi::vfs::curlResponses[{rep.GetBaseUrl(), "bytes=10-19"}] = {1, 2, 3};
  kodi::vfs::curlResponses[{rep.GetBaseUrl(), "bytes=30-39"}] = {4, 5, 6};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(stream.ReadSegment(first, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));
  ASSERT_TRUE(stream.ReadSegment(later, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{4, 5, 6}));
  EXPECT_EQ(cache->GetCachedRanges({stream.GetCacheTrackId()}),
            (std::vector<std::pair<uint64_t, uint64_t>>{{0, 1000000}, {3000000, 4000000}}));
  ASSERT_EQ(kodi::vfs::curlOpenRequests.size(), 2U);

  // Simulate a backward seek with the server now unavailable. Both complete
  // segments must replay with their original bytes, without another HTTP open.
  kodi::vfs::curlResponses.clear();
  ASSERT_TRUE(stream.ReadSegment(first, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{1, 2, 3}));
  ASSERT_TRUE(stream.ReadSegment(later, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{4, 5, 6}));
  EXPECT_EQ(kodi::vfs::curlOpenRequests.size(), 2U);
}

TEST_F(SegmentCacheReplay, PeriodRelativeVodAndEpochLiveRangesUseTheirOwnOrigins)
{
  for (const bool live : {false, true})
  {
    CacheReplayTree tree{live};
    auto firstPeriod = PLAYLIST::CPeriod::MakeUniquePtr();
    firstPeriod->SetIndex(1);
    firstPeriod->SetId("first");
    firstPeriod->SetTimescale(1000);
    firstPeriod->SetTlDuration(5000);
    tree.m_periods.push_back(std::move(firstPeriod));
    auto period = PLAYLIST::CPeriod::MakeUniquePtr();
    period->SetIndex(2);
    period->SetId("second");
    period->SetStart(5000);
    period->SetTimescale(1000);
    period->SetTlDuration(1000);
    tree.m_currentPeriod = period.get();
    tree.m_periods.push_back(std::move(period));
    tree.RefreshChaptersSnapshot();
    EXPECT_EQ(tree.GetPeriodStartTimeUs(2), 5000000U);
    auto cache = std::make_shared<ADP::SegmentCache>(ADP::SegmentCache::Mode::MEMORY, 32);
    tree.SetSegmentCacheForTest(cache);
    PLAYLIST::CAdaptationSet adp{tree.m_currentPeriod};
    adp.SetId("audio");
    adp.SetStreamType(PLAYLIST::StreamType::AUDIO);
    adp.SetLanguage("en");
    PLAYLIST::CRepresentation rep{&adp};
    rep.SetId("audio");
    rep.SetTimescale(1000);
    rep.SetBaseUrl("https://fixture.invalid/audio.m4s");
    const uint64_t epochMs = live ? 1700000000000 : 600000;
    CacheReplayStream stream{&tree, &adp, &rep, epochMs * 1000};
    PLAYLIST::CSegment segment;
    segment.startPTS_ = epochMs;
    segment.m_endPts = epochMs + 1000;
    segment.m_number = 5;
    rep.Timeline().Add(segment);
    kodi::vfs::curlResponses[{rep.GetBaseUrl(), ""}] = {7, 8};
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(stream.ReadSegment(segment, bytes));
    const uint64_t expectedBegin = live ? epochMs * 1000 : 5000000;
    EXPECT_EQ(
        cache->GetCachedRanges({stream.GetCacheTrackId()}),
        (std::vector<std::pair<uint64_t, uint64_t>>{{expectedBegin, expectedBegin + 1000000}}));

    adp.SetLanguage("ja");
    EXPECT_TRUE(cache->GetCachedRanges({stream.GetCacheTrackId(&rep)}).empty());
  }
}

TEST_F(SegmentCacheReplay, HlsVodReplaysDownloadedMediaAfterServerDisappears)
{
  CacheReplayHLSTree tree{false};
  auto period = PLAYLIST::CPeriod::MakeUniquePtr();
  period->SetIndex(0);
  period->SetSequence(2);
  period->SetStart(0);
  period->SetTimescale(1000);
  period->SetTlDuration(4000);
  tree.m_currentPeriod = period.get();
  tree.m_periods.push_back(std::move(period));
  tree.RefreshChaptersSnapshot();
  auto cache = std::make_shared<ADP::SegmentCache>(ADP::SegmentCache::Mode::MEMORY, 16);
  tree.SetSegmentCacheForTest(cache);
  PLAYLIST::CAdaptationSet adp{tree.m_currentPeriod};
  adp.SetId("muxed");
  adp.SetStreamType(PLAYLIST::StreamType::VIDEO_AUDIO);
  PLAYLIST::CRepresentation rep{&adp};
  rep.SetId("ts");
  rep.SetTimescale(1000);
  CacheReplayStream stream{&tree, &adp, &rep, 0};
  PLAYLIST::CSegment segment;
  segment.url = "https://fixture.invalid/hls/segment.ts";
  segment.startPTS_ = 0;
  segment.m_endPts = 4000;
  segment.m_number = 7;
  rep.Timeline().Add(segment);
  kodi::vfs::curlResponses[{segment.url, ""}] = {0x47, 1, 2, 3};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(stream.ReadSegment(segment, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{0x47, 1, 2, 3}));
  EXPECT_EQ(cache->GetCachedRanges({stream.GetCacheTrackId()}),
            (std::vector<std::pair<uint64_t, uint64_t>>{{0, 4000000}}));
  ASSERT_EQ(kodi::vfs::curlOpenRequests.size(), 1U);
  kodi::vfs::curlResponses.clear();
  ASSERT_TRUE(stream.ReadSegment(segment, bytes));
  EXPECT_EQ(bytes, (std::vector<uint8_t>{0x47, 1, 2, 3}));
  EXPECT_EQ(kodi::vfs::curlOpenRequests.size(), 1U);
}

TEST_F(SegmentCacheReplay, PrefetchedRepresentationUsesItsOwnVodOrigin)
{
  CacheReplayTree tree{false};
  auto period = PLAYLIST::CPeriod::MakeUniquePtr();
  period->SetIndex(0);
  period->SetTimescale(1000);
  period->SetTlDuration(1000);
  tree.m_currentPeriod = period.get();
  tree.m_periods.push_back(std::move(period));
  tree.RefreshChaptersSnapshot();
  auto cache = std::make_shared<ADP::SegmentCache>(ADP::SegmentCache::Mode::MEMORY, 32);
  tree.SetSegmentCacheForTest(cache);
  PLAYLIST::CAdaptationSet adp{tree.m_currentPeriod};
  adp.SetId("video");
  adp.SetStreamType(PLAYLIST::StreamType::VIDEO);
  PLAYLIST::CRepresentation active{&adp};
  active.SetId("2160p");
  active.SetTimescale(1000);
  PLAYLIST::CRepresentation next{&adp};
  next.SetId("1440p");
  next.SetTimescale(1000);
  next.SetBaseUrl("https://fixture.invalid/next.m4s");
  PLAYLIST::CSegment segment;
  segment.startPTS_ = 800000;
  segment.m_endPts = 801000;
  segment.m_number = 1;
  next.Timeline().Add(segment);
  CacheReplayStream stream{&tree, &adp, &active, 600000000};
  kodi::vfs::curlResponses[{next.GetBaseUrl(), ""}] = {1, 2};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(stream.ReadSegment(segment, bytes, &next));
  EXPECT_EQ(cache->GetCachedRanges({stream.GetCacheTrackId(&next)}),
            (std::vector<std::pair<uint64_t, uint64_t>>{{0, 1000000}}));
  EXPECT_TRUE(cache->GetCachedRanges({stream.GetCacheTrackId()}).empty());
}
