/*
 *  Copyright (C) 2021 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TestHelper.h"
#include "../decrypters/Helpers.h"
#include "../CompKodiProps.h"
#include "../SrvBroker.h"
#include "../common/SegmentCache.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>


class HLSTreeTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    CSrvBroker::GetInstance()->Initialize();
    m_reprChooser = new CTestRepresentationChooserDefault();
    tree = new HLSTestTree();
  }

  void TearDown() override
  {
    tree->Uninitialize();
    testHelper::effectiveUrl.clear();
    delete tree;
    tree = nullptr;
    delete m_reprChooser;
    m_reprChooser = nullptr;
  }

  void OpenTestFileMaster(std::string filePath)
  {
    OpenTestFileMaster(filePath, "http://foo.bar/" + filePath);
  }

  bool OpenTestFileMaster(std::string filePath, std::string url)
  {
    return OpenTestFileMaster(filePath, url, {});
  }

  bool OpenTestFileMaster(std::string filePath,
                          std::string url,
                          std::map<std::string, std::string> manifestHeaders)
  {
    testHelper::testFile = filePath;

    CSrvBroker::GetInstance()->InitStage1({});

    // Download the manifest
    UTILS::CURL::HTTPResponse resp;
    if (!testHelper::DownloadFile(url, {}, {}, resp))
    {
      LOG::Log(LOGERROR, "Cannot download \"%s\" DASH manifest file.", url.c_str());
      return false;
    }

    ADP::KODI_PROPS::ChooserProps chooserProps;
    m_reprChooser->Initialize(chooserProps);
    // We set the download speed to calculate the initial network bandwidth
    m_reprChooser->SetDownloadSpeed(500000);

    tree->Configure(m_reprChooser, "");

    // Parse the manifest
    if (!tree->Open(resp.effectiveUrl, resp.headers, resp.data))
    {
      LOG::Log(LOGERROR, "Cannot open \"%s\" HLS manifest.", url.c_str());
      return false;
    }

    tree->PostOpen();
    tree->m_currentAdpSet = tree->m_periods[0]->GetAdaptationSets()[0].get();
    tree->m_currentRepr = tree->m_currentAdpSet->GetRepresentations()[0].get();
    return true;
  }

  bool OpenTestFileVariant(std::string filePath,
                           std::string url,
                           PLAYLIST::CPeriod* per,
                           PLAYLIST::CAdaptationSet* adp,
                           PLAYLIST::CRepresentation* rep)
  {
    if (!url.empty())
      rep->SetSourceUrl(url);

    testHelper::testFile = filePath;
    return tree->PrepareRepresentation(per, adp, rep);
  }

  adaptive::CHLSTree* tree;
  CHOOSER::IRepresentationChooser* m_reprChooser{nullptr};
};

TEST_F(HLSTreeTest, CachedLiveTimelineIncludesInitialServerWindow)
{
  ASSERT_TRUE(OpenTestFileMaster("hls/1v_master.m3u8", "https://foo.bar/master.m3u8"));
  auto* period = tree->m_currentPeriod;
  auto* adp = tree->m_currentAdpSet;
  auto* rep = tree->m_currentRepr;
  ASSERT_TRUE(OpenTestFileVariant("hls/cache_live_window.m3u8",
                                  "https://foo.bar/live/variant.m3u8", period, adp, rep));
  ASSERT_GT(rep->Timeline().GetSize(), 1U);
  tree->SetSegmentCacheForTest(
      std::make_shared<ADP::SegmentCache>(ADP::SegmentCache::Mode::MEMORY, 1024));

  TestAdaptiveStream stream{tree, adp, rep};
  ASSERT_TRUE(stream.start_stream());
  ASSERT_TRUE(rep->current_segment_.has_value());
  EXPECT_GT(rep->current_segment_->startPTS_, rep->Timeline().GetFront()->startPTS_);

  const uint64_t firstPts = rep->Timeline().GetFront()->startPTS_ * 1000000 /
                            rep->GetTimescale();
  const uint64_t lastPts = rep->Timeline().GetBack()->m_endPts * 1000000 /
                           rep->GetTimescale();
  EXPECT_TRUE(tree->IsLive());
  EXPECT_EQ(tree->GetCachePlaybackStartPts(), firstPts);
  EXPECT_EQ(tree->GetCachedLiveDurationMs(), (lastPts - firstPts) / 1000);
}


TEST_F(HLSTreeTest, CalculateSourceUrl)
{
  OpenTestFileMaster("hls/1a2v_master.m3u8", "https://foo.bar/master.m3u8?param=foo");

  bool ret = OpenTestFileVariant("hls/fmp4_noenc_v_stream_2.m3u8", "https://foo.bar/stream_2/out.m3u8",
                                 tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);
  EXPECT_EQ(ret, true);

  std::string rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();
  EXPECT_EQ(rep_url, "https://foo.bar/stream_2/out.m3u8");
}

TEST_F(HLSTreeTest, CalculateSourceUrlFromRedirectedMasterRelativeUri)
{
  testHelper::effectiveUrl = "https://foo.bar/master.m3u8";

  OpenTestFileMaster("hls/1a2v_master.m3u8", "https://baz.qux/master.m3u8");

  std::string rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  EXPECT_EQ(rep_url, "https://foo.bar/stream_2/out.m3u8");

  bool ret = OpenTestFileVariant("hls/fmp4_noenc_v_stream_2.m3u8", "https://foo.bar/stream_2/out.m3u8", tree->m_currentPeriod,
                                 tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();
  EXPECT_EQ(rep_url, "https://foo.bar/stream_2/out.m3u8");
}

TEST_F(HLSTreeTest, CalculateSourceUrlFromRedirectedVariantAbsoluteUri)
{
  OpenTestFileMaster("hls/redirect_absolute_1v_master.m3u8", "https://baz.qux/master.m3u8");

  std::string rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  EXPECT_EQ(rep_url, "https://bit.ly/abcd");

  testHelper::effectiveUrl = "https://foo.bar/stream_2/out.m3u8";

  bool ret = OpenTestFileVariant(
      "hls/fmp4_noenc_v_stream_2.m3u8", "https://bit.ly/abcd",
      tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();
  EXPECT_EQ(rep_url, "https://bit.ly/abcd");
}

TEST_F(HLSTreeTest, CalculateSourceUrlFromRedirectedMasterAndRedirectedVariantAbsoluteUri)
{
  testHelper::effectiveUrl = "https://baz.qux/master.m3u8";

  OpenTestFileMaster("hls/redirect_absolute_1v_master.m3u8", "https://link.to/1234");

  std::string rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  EXPECT_EQ(rep_url, "https://bit.ly/abcd");

  testHelper::effectiveUrl = "https://foo.bar/stream_2/out.m3u8";

  bool ret = OpenTestFileVariant(
      "hls/fmp4_noenc_v_stream_2.m3u8", "https://bit.ly/abcd", tree->m_currentPeriod,
      tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();
  EXPECT_EQ(rep_url, "https://bit.ly/abcd");
}

TEST_F(HLSTreeTest,
       CalculateSourceUrlFromRedirectedMasterAndRedirectedVariantAbsoluteUriSameDomains)
{
  testHelper::effectiveUrl = "https://baz.qux/master.m3u8";

  OpenTestFileMaster("hls/redirect_absolute_1v_master.m3u8", "https://bit.ly/1234");

  std::string rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  EXPECT_EQ(rep_url, "https://bit.ly/abcd");

  testHelper::effectiveUrl = "https://foo.bar/stream_2/out.m3u8";

  bool ret = OpenTestFileVariant(
      "hls/fmp4_noenc_v_stream_2.m3u8", "https://bit.ly/abcd", tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  rep_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();
  EXPECT_EQ(rep_url, "https://bit.ly/abcd");
}

TEST_F(HLSTreeTest, OpenVariant)
{
  OpenTestFileMaster("hls/1a2v_master.m3u8", "https://foo.bar/master.m3u8");

  bool ret = OpenTestFileVariant(
      "hls/fmp4_noenc_v_stream_2.m3u8", "https://foo.bar/stream_2.m3u8", tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);
  EXPECT_EQ(tree->base_url_, "https://foo.bar/");
}

TEST_F(HLSTreeTest, ParseKeyUriStartingWithSlash)
{
  OpenTestFileMaster("hls/1v_master.m3u8", "https://foo.bar/hls/video/stream_name/master.m3u8");

  bool ret = OpenTestFileVariant(
      "hls/ts_aes_keyuriwithslash_stream_0.m3u8",
      "https://foo.bar/hls/video/stream_name/chunklist.m3u8", tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  auto& timeline = tree->m_currentRepr->Timeline();
  EXPECT_EQ(timeline.GetSize(), 6);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo().has_value(), true);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo()->keyUrl, "https://foo.bar/hls/key/key.php?stream=stream_name");
}

TEST_F(HLSTreeTest, ParseKeyUriStartingWithSlashFromRedirect)
{
  testHelper::effectiveUrl = "https://foo.bar/hls/video/stream_name/master.m3u8";

  OpenTestFileMaster("hls/1v_master.m3u8", "https://baz.qux/hls/video/stream_name/master.m3u8");

  bool ret = OpenTestFileVariant(
      "hls/ts_aes_keyuriwithslash_stream_0.m3u8",
      "https://foo.bar/hls/video/stream_name/chunklist.m3u8", tree->m_currentPeriod,
      tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  auto& timeline = tree->m_currentRepr->Timeline();
  EXPECT_EQ(timeline.GetSize(), 6);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo().has_value(), true);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo()->keyUrl, "https://foo.bar/hls/key/key.php?stream=stream_name");
}

TEST_F(HLSTreeTest, ParseKeyUriAbsolute)
{
  OpenTestFileMaster("hls/1v_master.m3u8", "https://foo.bar/hls/video/stream_name/master.m3u8");

  bool ret = OpenTestFileVariant(
      "hls/ts_aes_keyuriabsolute_stream_0.m3u8",
      "https://foo.bar/hls/video/stream_name/chunklist.m3u8", tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  auto& timeline = tree->m_currentRepr->Timeline();
  EXPECT_EQ(timeline.GetSize(), 6);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo().has_value(), true);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo()->keyUrl, "https://foo.bar/hls/key/key.php?stream=stream_name");
}

TEST_F(HLSTreeTest, ParseKeyUriRelative)
{
  OpenTestFileMaster("hls/1v_master.m3u8", "https://foo.bar/hls/video/stream_name/master.m3u8");

  bool ret = OpenTestFileVariant(
      "hls/ts_aes_keyurirelative_stream_0.m3u8",
      "https://foo.bar/hls/video/stream_name/chunklist.m3u8", tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  auto& timeline = tree->m_currentRepr->Timeline();
  EXPECT_EQ(timeline.GetSize(), 6);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo().has_value(), true);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo()->keyUrl, "https://foo.bar/hls/key/key.php?stream=stream_name");
}

TEST_F(HLSTreeTest, ParseKeyUriRelativeFromRedirect)
{
  testHelper::effectiveUrl = "https://foo.bar/hls/video/stream_name/master.m3u8";

  OpenTestFileMaster("hls/1v_master.m3u8", "https://baz.qux/hls/video/stream_name/master.m3u8");
  std::string var_download_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl(); // https://baz.qux/hls/video/stream_name/ts_aes_uriwithslash_chunklist.m3u8

  bool ret = OpenTestFileVariant(
      "hls/ts_aes_keyurirelative_stream_0.m3u8",
      var_download_url,
      tree->m_currentPeriod,
      tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);

  auto& timeline = tree->m_currentRepr->Timeline();
  EXPECT_EQ(timeline.GetSize(), 6);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo().has_value(), true);
  EXPECT_EQ(timeline.GetFront()->AESKeyInfo()->keyUrl, "https://foo.bar/hls/key/key.php?stream=stream_name");
}

TEST_F(HLSTreeTest, PtsSetInMultiPeriod)
{
  OpenTestFileMaster("hls/1a2v_master.m3u8", "https://foo.bar/master.m3u8");
  std::string var_download_url = tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  {
    auto& periodFirst = tree->m_periods[0];

    bool ret =
        OpenTestFileVariant("hls/disco_fmp4_noenc_v_stream_1.m3u8", var_download_url,
                            periodFirst.get(), periodFirst->GetAdaptationSets()[0].get(),
                            periodFirst->GetAdaptationSets()[0]->GetRepresentations()[0].get());

    EXPECT_EQ(ret, true);

    auto& periodSecond = tree->m_periods[1];
    auto& adp0rep1 = periodSecond->GetAdaptationSets()[0]->GetRepresentations()[0];
    auto adp0rep1seg1 = adp0rep1->Timeline().GetFront();
    EXPECT_EQ(adp0rep1seg1->startPTS_, 0);
  }
  {
    auto& periodFirst = tree->m_periods[0];
    var_download_url =
        tree->m_currentPeriod->GetAdaptationSets()[1]->GetRepresentations()[0]->GetSourceUrl();
    bool ret =
        OpenTestFileVariant("hls/disco_fmp4_noenc_a_stream_0.m3u8", var_download_url,
                              periodFirst.get(), periodFirst->GetAdaptationSets()[1].get(),
                              periodFirst->GetAdaptationSets()[1]->GetRepresentations()[0].get());

    EXPECT_EQ(ret, true);

    auto& periodSecond = tree->m_periods[1];
    auto& adp1rep0 = periodSecond->GetAdaptationSets()[1]->GetRepresentations()[0];
    auto adp1rep0seg1 = adp1rep0->Timeline().GetFront();
    EXPECT_EQ(adp1rep0seg1->startPTS_, 0);
  }
}

TEST_F(HLSTreeTest, MultipleEncryptionSequence)
{
  testHelper::effectiveUrl = "https://foo.bar/hls/video/stream_name/master.m3u8";

  OpenTestFileMaster("hls/encrypt_master.m3u8", "https://baz.qux/hls/video/stream_name/master.m3u8");
  std::string var_download_url =
      tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  bool ret = OpenTestFileVariant("hls/encrypt_seq_stream.m3u8", var_download_url,
                                 tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);
  auto& periods = tree->m_periods;
  EXPECT_EQ(periods.size(), 3);

  // the first period have unencrypted segments
  auto& p1repr = periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0];
  EXPECT_EQ(p1repr->Timeline().GetSize(), 4);
  for (auto& seg : p1repr->Timeline())
  {
    EXPECT_EQ(seg.AESKeyInfo().has_value(), false);
  }

  // the second period have segments AES-128 encrypted
  auto& p2repr = periods[1]->GetAdaptationSets()[0]->GetRepresentations()[0];
  EXPECT_EQ(p2repr->Timeline().GetSize(), 2);
  for (auto& seg : p2repr->Timeline())
  {
    EXPECT_EQ(seg.AESKeyInfo().has_value(), true);
    EXPECT_EQ(seg.AESKeyInfo()->keyUrl.empty(), false);
    EXPECT_EQ(seg.AESKeyInfo()->iv.empty(), false);
  }

  // the third period have unencrypted segments
  auto& p3repr = periods[2]->GetAdaptationSets()[0]->GetRepresentations()[0];
  EXPECT_EQ(p3repr->Timeline().GetSize(), 4);
  for (auto& seg : p3repr->Timeline())
  {
    EXPECT_EQ(seg.AESKeyInfo().has_value(), false);
  }
}

TEST_F(HLSTreeTest, MultipleEncryptionSequenceDrmNoKSMaster)
{
  // master manifest contains EXT-X-SESSION-KEY that can be used to check supported DRM's
  // but currently this check is not performed, so the open operation return true
  testHelper::effectiveUrl = "https://foo.bar/hls/video/stream_name/master.m3u8";

  bool ret = OpenTestFileMaster("hls/encrypt_master_drm.m3u8",
                                "https://baz.qux/hls/video/stream_name/master.m3u8", {});
  EXPECT_EQ(ret, true);
}

TEST_F(HLSTreeTest, MultipleEncryptionSequenceDrm)
{
  // The child manifest support multiple DRM's, but have an unsupported KEYFORMAT that should be ignored
  // the representation's of each period must contains the DRM info
  testHelper::effectiveUrl = "https://foo.bar/hls/video/stream_name/master.m3u8";

  bool ret = OpenTestFileMaster("hls/encrypt_master.m3u8",
                                "https://baz.qux/hls/video/stream_name/master.m3u8", {});

  EXPECT_EQ(ret, true);

  std::string var_download_url =
      tree->m_currentPeriod->GetAdaptationSets()[0]->GetRepresentations()[0]->GetSourceUrl();

  ret = OpenTestFileVariant("hls/encrypt_seq_stream_drm.m3u8", var_download_url,
                            tree->m_currentPeriod, tree->m_currentAdpSet, tree->m_currentRepr);

  EXPECT_EQ(ret, true);
  auto& periods = tree->m_periods;
  EXPECT_EQ(periods.size(), 2);

  auto& p1rep = periods[0]->GetAdaptationSets()[0]->GetRepresentations()[0];
  EXPECT_EQ(p1rep->DrmInfos().size(), 2);
  std::set<std::string_view> ks1{DRM::KS_WIDEVINE, DRM::KS_PLAYREADY};
  for (auto& drmInfo : p1rep->DrmInfos()) // Do not rely on the order of DrmInfos items
  {
    ks1.erase(drmInfo.keySystem);
  }
  EXPECT_TRUE(ks1.empty());

  auto& p2rep = periods[1]->GetAdaptationSets()[0]->GetRepresentations()[0];
  EXPECT_EQ(p2rep->DrmInfos().size(), 2);
  std::set<std::string_view> ks2{DRM::KS_WIDEVINE, DRM::KS_PLAYREADY};
  for (auto& drmInfo : p1rep->DrmInfos()) // Do not rely on the order of DrmInfos items
  {
    ks2.erase(drmInfo.keySystem);
  }
  EXPECT_TRUE(ks2.empty());
}

TEST_F(HLSTreeTest, CachedAesSegmentReplaysThroughDecryptor)
{
  tree->Configure(m_reprChooser, "");

  // First two AES-128-CBC blocks from NIST SP 800-38A, example F.2.1.
  const std::vector<uint8_t> encrypted{0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
                                       0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
                                       0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
                                       0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2};
  const std::vector<uint8_t> expected{0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                      0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
                                      0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
                                      0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51};
  const std::vector<ADP::SegmentCache::Chunk> chunks{{16, false}, {16, false}};
  ADP::SegmentCache::Key cacheKey{
      "segment.ts", {}, 1, 0, "key-uri", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
  const auto root = std::filesystem::temp_directory_path() /
                    ("isa-hls-aes-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  for (const auto mode : {ADP::SegmentCache::Mode::MEMORY, ADP::SegmentCache::Mode::DISK})
  {
    ADP::SegmentCache cache{mode, 64, root};
    ASSERT_TRUE(cache.IsAvailable());
    cache.Put(cacheKey, encrypted, chunks);

    if (mode == ADP::SegmentCache::Mode::DISK)
    {
      const auto sessionDirectory = *std::filesystem::directory_iterator(root);
      const auto file = *std::filesystem::directory_iterator(sessionDirectory.path());
      std::ifstream stored(file.path(), std::ios::binary);
      const std::vector<uint8_t> storedBytes{std::istreambuf_iterator<char>(stored), {}};
      EXPECT_EQ(storedBytes, encrypted);
    }

    std::vector<uint8_t> cachedData;
    std::vector<ADP::SegmentCache::Chunk> cachedChunks;
    ASSERT_TRUE(cache.Get(cacheKey, cachedData, &cachedChunks));
    ASSERT_EQ(cachedChunks, chunks);

    std::optional<CAesKeyInfo> aesKey{CAesKeyInfo{{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                                   0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c},
                                                  cacheKey.iv,
                                                  cacheKey.keyUrl}};
    uint8_t iv[16]{};
    std::vector<uint8_t> replayed;
    size_t offset{0};
    for (const auto& chunk : cachedChunks)
    {
      std::vector<uint8_t> output;
      tree->OnDataArrived(cacheKey.number, aesKey, iv, cachedData.data() + offset, chunk.size,
                          output, replayed.size(), chunk.isLast);
      replayed.insert(replayed.end(), output.begin(), output.end());
      offset += chunk.size;
    }
    EXPECT_EQ(replayed, expected);
  }
  std::filesystem::remove(root);
}
