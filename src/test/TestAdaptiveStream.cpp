/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "../CompKodiProps.h"
#include "../SrvBroker.h"
#include "TestHelper.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>

using namespace std::chrono_literals;

namespace
{
// Model a CURL open/read that has not returned. Cancellation must wake a
// demuxer even while the network worker still owns its download mutex.
class StalledAdaptiveStream : public adaptive::AdaptiveStream
{
public:
  StalledAdaptiveStream(adaptive::AdaptiveTree* tree,
                        PLAYLIST::CAdaptationSet* adp,
                        PLAYLIST::CRepresentation* rep)
    : adaptive::AdaptiveStream(tree, adp, rep)
  {
  }

  ~StalledAdaptiveStream() override
  {
    ReleaseNetwork();
    // Join before the synchronization members below are destroyed.
    Stop();
    Dispose();
  }

  bool WaitForNetwork()
  {
    std::unique_lock<std::mutex> lock(m_networkMutex);
    return m_networkCv.wait_for(lock, 2s, [this] { return m_networkStarted; });
  }

  bool NetworkFinished()
  {
    std::lock_guard<std::mutex> lock(m_networkMutex);
    return m_networkFinished;
  }

  void ReleaseNetwork()
  {
    std::lock_guard<std::mutex> lock(m_networkMutex);
    m_releaseNetwork = true;
    m_networkCv.notify_all();
  }

  void StallNextNetwork()
  {
    // Cancellation has stopped queue processing. Wait until the cancelled
    // download is retired before stalling the next seek destination.
    std::lock_guard<std::mutex> workerLock(thread_data_->mutexWorker);
    std::lock_guard<std::mutex> networkLock(m_networkMutex);
    m_networkStarted = false;
    m_networkFinished = false;
    m_releaseNetwork = false;
  }

  void WaitForDownloadRetired()
  {
    std::lock_guard<std::mutex> lock(thread_data_->mutexWorker);
  }

  void FailDownloads() { m_failDownloads = true; }
  void AllowFirstDownload() { m_downloadsBeforeStall = 1; }
  size_t DownloadAttempts() const { return m_downloadAttempts; }

protected:
  bool DownloadSegment(const DownloadInfo& downloadInfo) override
  {
    const size_t attempt = ++m_downloadAttempts;
    if (m_failDownloads)
      return false;

    if (attempt > m_downloadsBeforeStall)
    {
      std::unique_lock<std::mutex> lock(m_networkMutex);
      m_networkStarted = true;
      m_networkCv.notify_all();
      m_networkCv.wait(lock, [this] { return m_releaseNetwork; });
      m_networkFinished = true;
    }

    {
      std::lock_guard<std::mutex> lock(thread_data_->mutexRW);
      if (thread_data_->State() == THREADDATA::ThState::STOPPED)
        return false;

      downloadInfo.m_segmentBuffer->AppendBuffer(std::vector<uint8_t>(16, 0x5a));
    }
    thread_data_->cvRW.notify_all();
    return true;
  }

private:
  std::mutex m_networkMutex;
  std::condition_variable m_networkCv;
  bool m_networkStarted{false};
  bool m_networkFinished{false};
  bool m_releaseNetwork{false};
  std::atomic<bool> m_failDownloads{false};
  std::atomic<size_t> m_downloadAttempts{0};
  std::atomic<size_t> m_downloadsBeforeStall{0};
};

class CancellationTestTree : public DASHTestTree
{
public:
  void SetLiveForTest(bool live) { m_isLive = live; }
};

class AdaptiveStreamCancellationTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    CSrvBroker::GetInstance()->Initialize();
    CSrvBroker::GetInstance()->InitStage1({});
    m_chooser = std::make_unique<CTestRepresentationChooserDefault>();
    m_chooser->Initialize({});
    m_chooser->SetDownloadSpeed(500000);
    m_tree = std::make_unique<CancellationTestTree>();
    m_tree->Configure(m_chooser.get(), "");

    const std::string manifest = R"(
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT60S">
  <Period id="p0" start="PT0S">
    <AdaptationSet contentType="video" mimeType="video/mp4">
      <SegmentTemplate timescale="1" duration="2" media="$Number$.m4s" startNumber="1" />
      <Representation id="video" bandwidth="300000" codecs="avc1.64001e" width="640" height="360" />
    </AdaptationSet>
  </Period>
</MPD>)";
    ASSERT_TRUE(m_tree->Open("https://example.test/manifest.mpd", {}, manifest));
    m_tree->PostOpen();
    auto adp = m_tree->m_currentPeriod->GetAdaptationSets().front().get();
    m_stream = std::make_unique<StalledAdaptiveStream>(
        m_tree.get(), adp, m_tree->GetRepChooser()->GetRepresentation(adp));
    ASSERT_TRUE(m_stream->start_stream());
  }

  void TearDown() override
  {
    m_stream.reset();
    m_tree->Uninitialize();
    m_tree.reset();
    m_chooser.reset();
  }

  void ExpectInterruptedRead(std::future<uint32_t>& pending, bool abort = false)
  {
    const bool networkStarted = m_stream->WaitForNetwork();
    EXPECT_TRUE(networkStarted);
    EXPECT_EQ(pending.wait_for(30ms), std::future_status::timeout);
    if (abort)
      m_tree->RequestAbort();
    else
      m_stream->CancelPendingRead();
    const auto cancelled = pending.wait_for(500ms);
    EXPECT_EQ(cancelled, std::future_status::ready);
    EXPECT_FALSE(m_stream->NetworkFinished());
    // Always release before retrieving a result so failures cannot hang teardown.
    m_stream->ReleaseNetwork();
    EXPECT_EQ(pending.get(), 0u);
  }

  std::unique_ptr<CTestRepresentationChooserDefault> m_chooser;
  std::unique_ptr<CancellationTestTree> m_tree;
  std::unique_ptr<StalledAdaptiveStream> m_stream;
};

TEST_F(AdaptiveStreamCancellationTest, ReadCompletesBeforeStalledNetworkAndSeekRestarts)
{
  std::array<uint8_t, 16> data{};
  auto pending = std::async(std::launch::async, [&] {
    return m_stream->read(data.data(), data.size());
  });
  ExpectInterruptedRead(pending);
  // A cancelled target remains in the queue as INVALID. Re-seeking to that
  // same target must re-download it instead of preserving the invalid buffer.
  m_stream->WaitForDownloadRetired();

  ASSERT_TRUE(m_stream->getRepresentation()->IsEnabled());
  ASSERT_TRUE(m_stream->seek_time(0));
  EXPECT_EQ(m_stream->read(data.data(), data.size()), data.size());
  EXPECT_EQ(data.front(), 0x5a);
}

TEST_F(AdaptiveStreamCancellationTest, ByteSeekCompletesBeforeStalledNetwork)
{
  // Queue the first segment without requesting any bytes.
  EXPECT_EQ(m_stream->read(nullptr, 0), 0u);
  auto pending = std::async(std::launch::async, [&] {
    bool isEos{false};
    return static_cast<uint32_t>(m_stream->seek(8, isEos));
  });
  ExpectInterruptedRead(pending);
}

TEST_F(AdaptiveStreamCancellationTest, SameTargetSeekDiscardsCancelledPrefetch)
{
  std::array<uint8_t, 16> data{};
  m_stream->AllowFirstDownload();
  ASSERT_EQ(m_stream->read(data.data(), data.size()), data.size());
  EXPECT_TRUE(m_stream->WaitForNetwork());
  // The target is downloaded, but the following queued segment is cancelled.
  // Preserving only the valid target would keep an INVALID prefetch behind it.
  m_stream->CancelPendingRead();
  m_stream->ReleaseNetwork();
  m_stream->WaitForDownloadRetired();

  ASSERT_TRUE(m_stream->seek_time(0));
  EXPECT_EQ(m_stream->read(data.data(), data.size()), data.size());
  EXPECT_EQ(m_stream->read(data.data(), data.size()), data.size());
  EXPECT_EQ(data.front(), 0x5a);
}

TEST_F(AdaptiveStreamCancellationTest, RepeatedCancelledSeeksCanRestartPlayback)
{
  std::array<uint8_t, 16> data{};
  for (size_t round = 0; round < 5; ++round)
  {
    if (round > 0)
    {
      m_stream->StallNextNetwork();
      ASSERT_TRUE(m_stream->seek_time(round * 10));
    }
    auto pending = std::async(std::launch::async, [&] {
      return m_stream->read(data.data(), data.size());
    });
    ExpectInterruptedRead(pending);
  }

  ASSERT_TRUE(m_stream->seek_time(0));
  EXPECT_EQ(m_stream->read(data.data(), data.size()), data.size());
  EXPECT_EQ(data.front(), 0x5a);
}

TEST_F(AdaptiveStreamCancellationTest, FullBufferReadCompletesBeforeStalledNetwork)
{
  std::vector<uint8_t> data;
  auto pending = std::async(std::launch::async, [&] {
    return static_cast<uint32_t>(m_stream->ReadFullBuffer(data));
  });
  ExpectInterruptedRead(pending);
  EXPECT_TRUE(data.empty());
}

TEST_F(AdaptiveStreamCancellationTest, AbortUnblocksReadWhileNetworkRemainsStalled)
{
  std::array<uint8_t, 16> data{};
  auto pending = std::async(std::launch::async, [&] {
    return m_stream->read(data.data(), data.size());
  });
  ExpectInterruptedRead(pending, true);
}

TEST_F(AdaptiveStreamCancellationTest, AbortUnblocksFullBufferWhileNetworkRemainsStalled)
{
  std::vector<uint8_t> data;
  auto pending = std::async(std::launch::async, [&] {
    return static_cast<uint32_t>(m_stream->ReadFullBuffer(data));
  });
  ExpectInterruptedRead(pending, true);
  EXPECT_TRUE(data.empty());
}

TEST_F(AdaptiveStreamCancellationTest, FailedSeekDoesNotRepeatStalledDestination)
{
  std::array<uint8_t, 16> data{};
  m_tree->SetLiveForTest(true);
  m_stream->FailDownloads();
  m_stream->SetSeekMode(true);
  EXPECT_EQ(m_stream->read(data.data(), data.size()), 0u);
  EXPECT_EQ(m_stream->DownloadAttempts(), 1u);
}

TEST_F(AdaptiveStreamCancellationTest, NormalLivePlaybackRetainsDownloadRetries)
{
  std::array<uint8_t, 16> data{};
  m_tree->SetLiveForTest(true);
  m_stream->FailDownloads();
  EXPECT_EQ(m_stream->read(data.data(), data.size()), 0u);
  EXPECT_EQ(m_stream->DownloadAttempts(), 6u);
}

TEST_F(AdaptiveStreamCancellationTest, VodSeekRetainsDownloadRetries)
{
  std::array<uint8_t, 16> data{};
  m_stream->FailDownloads();
  m_stream->SetSeekMode(true);
  EXPECT_EQ(m_stream->read(data.data(), data.size()), 0u);
  EXPECT_EQ(m_stream->DownloadAttempts(), 3u);
}
} // namespace
