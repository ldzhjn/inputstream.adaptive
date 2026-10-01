/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ADP
{
// Shared by all streams in one playback session. Only complete media segments are inserted.
class SegmentCache
{
public:
  enum class Mode
  {
    MEMORY,
    DISK
  };

  struct Key
  {
    std::string url;
    std::map<std::string, std::string> headers;
    uint64_t number;
    uint64_t startPts;
    std::string keyUrl;
    std::vector<uint8_t> iv;
    std::string periodId;
    uint32_t periodSequence{0};

    bool operator==(const Key& other) const
    {
      return url == other.url && headers == other.headers && number == other.number &&
             startPts == other.startPts && keyUrl == other.keyUrl && iv == other.iv &&
             periodId == other.periodId && periodSequence == other.periodSequence;
    }
  };

  struct Chunk
  {
    size_t size;
    bool isLast;

    bool operator==(const Chunk& other) const
    {
      return size == other.size && isLast == other.isLast;
    }
  };

  SegmentCache(Mode mode, size_t maxBytes, const std::filesystem::path& diskRoot = {});
  ~SegmentCache();

  SegmentCache(const SegmentCache&) = delete;
  SegmentCache& operator=(const SegmentCache&) = delete;

  bool IsAvailable() const { return m_available; }
  size_t MaxBytes() const { return m_maxBytes; }
  bool Get(const Key& key, std::vector<uint8_t>& data, std::vector<Chunk>* chunks = nullptr);
  void Put(Key key, std::vector<uint8_t> data, std::vector<Chunk> chunks = {});

private:
  struct Entry
  {
    Key key;
    size_t size;
    std::vector<uint8_t> data;
    std::vector<Chunk> chunks;
    std::filesystem::path path;
  };

  std::list<Entry>::iterator Find(const Key& key);
  void Erase(std::list<Entry>::iterator entry);

  const Mode m_mode;
  const size_t m_maxBytes;
  bool m_available{true};
  size_t m_sizeBytes{0};
  uint64_t m_nextFileId{0};
  std::filesystem::path m_diskDirectory;
  std::list<Entry> m_entries;
  std::mutex m_mutex;
};
} // namespace ADP
