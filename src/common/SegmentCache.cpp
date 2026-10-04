/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SegmentCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <utility>

using namespace ADP;

SegmentCache::SegmentCache(Mode mode, size_t maxBytes, const std::filesystem::path& diskRoot)
  : m_mode(mode), m_maxBytes(maxBytes)
{
  if (maxBytes == 0)
  {
    m_available = false;
    return;
  }

  if (mode == Mode::DISK)
  {
    std::error_code ec;
    // Only native local paths are supported by std::fstream. A Kodi VFS URL
    // must never become an accidental relative directory such as "smb:".
    if (diskRoot.empty() || !diskRoot.is_absolute() ||
        diskRoot.string().find("://") != std::string::npos)
    {
      m_available = false;
      return;
    }
    std::filesystem::create_directories(diskRoot, ec);
    if (ec)
    {
      m_available = false;
      return;
    }

    // A private directory prevents entries from another playback from being reused.
    static std::atomic<uint64_t> nextSessionId{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 10; ++attempt)
    {
      auto path =
          diskRoot / ("session-" + std::to_string(stamp) + "-" + std::to_string(nextSessionId++));
      ec.clear();
      if (std::filesystem::create_directory(path, ec))
      {
        m_diskDirectory = std::move(path);
#ifndef _WIN32
        std::filesystem::permissions(m_diskDirectory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        if (ec)
        {
          std::filesystem::remove(m_diskDirectory, ec);
          m_diskDirectory.clear();
          m_available = false;
        }
#endif
        return;
      }
      if (ec != std::errc::file_exists)
        break;
    }
    m_available = false;
  }
}

SegmentCache::~SegmentCache()
{
  if (!m_diskDirectory.empty())
  {
    std::error_code ec;
    std::filesystem::remove_all(m_diskDirectory, ec);
  }
}

std::list<SegmentCache::Entry>::iterator SegmentCache::Find(const Key& key)
{
  return std::find_if(m_entries.begin(), m_entries.end(),
                      [&key](const Entry& entry) { return entry.key == key; });
}

void SegmentCache::Erase(std::list<Entry>::iterator entry)
{
  m_sizeBytes -= entry->size;
  if (!entry->path.empty())
  {
    std::error_code ec;
    std::filesystem::remove(entry->path, ec);
  }
  m_entries.erase(entry);
}

bool SegmentCache::Get(const Key& key,
                       std::vector<uint8_t>& data,
                       std::vector<Chunk>* chunks,
                       std::optional<PLAYLIST::CSegment>* segment)
{
  if (!m_available)
    return false;

  std::lock_guard lock(m_mutex);
  auto entry = Find(key);
  if (entry == m_entries.end())
  {
    // A refreshed live manifest may rotate URL signatures while retaining
    // the same segment. Its immutable media identity is enough for replay.
    entry = std::find_if(m_entries.begin(), m_entries.end(),
                         [&](const Entry& candidate)
                         {
                           const Key& old = candidate.key;
                           return candidate.segment && !candidate.segment->IsInitialization() &&
                                  old.number == key.number && old.startPts == key.startPts &&
                                  old.iv == key.iv &&
                                  old.periodId == key.periodId &&
                                  old.periodSequence == key.periodSequence &&
                                  old.periodStart == key.periodStart &&
                                  old.adaptationId == key.adaptationId &&
                                  old.representationId == key.representationId;
                         });
  }
  if (entry == m_entries.end())
    return false;

  if (m_mode == Mode::MEMORY)
  {
    data = entry->data;
  }
  else
  {
    std::ifstream file(entry->path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(entry->size))
    {
      file.close();
      Erase(entry);
      return false;
    }
    file.seekg(0);
    data.resize(entry->size);
    if (!file.read(reinterpret_cast<char*>(data.data()), data.size()))
    {
      file.close();
      data.clear();
      Erase(entry);
      return false;
    }
  }

  if (chunks)
    *chunks = entry->chunks;
  if (segment)
    *segment = entry->segment;
  m_entries.splice(m_entries.begin(), m_entries, entry);
  return true;
}

void SegmentCache::Put(Key key,
                       std::vector<uint8_t> data,
                       std::vector<Chunk> chunks,
                       std::optional<PLAYLIST::CSegment> segment)
{
  if (!m_available || data.empty() || data.size() > m_maxBytes)
    return;

  if (chunks.empty())
    chunks.push_back({data.size(), false});
  size_t chunkBytes{0};
  for (const auto& chunk : chunks)
  {
    if (chunk.size > data.size() - chunkBytes)
      return;
    chunkBytes += chunk.size;
  }
  if (chunkBytes != data.size())
    return;

  std::lock_guard lock(m_mutex);
  auto old = Find(key);
  if (old != m_entries.end())
    Erase(old);

  while (m_sizeBytes + data.size() > m_maxBytes)
    Erase(std::prev(m_entries.end()));

  Entry entry{std::move(key), data.size(), {}, std::move(chunks), {}, std::move(segment)};
  if (m_mode == Mode::MEMORY)
  {
    entry.data = std::move(data);
  }
  else
  {
    entry.path = m_diskDirectory / (std::to_string(m_nextFileId++) + ".seg");
    std::ofstream file(entry.path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(data.data()), data.size()))
    {
      file.close();
      std::error_code ec;
      std::filesystem::remove(entry.path, ec);
      return;
    }
    file.close();
    if (!file)
    {
      std::error_code ec;
      std::filesystem::remove(entry.path, ec);
      return;
    }
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(
        entry.path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
#endif
  }

  m_sizeBytes += entry.size;
  m_entries.push_front(std::move(entry));
}

std::vector<PLAYLIST::CSegment> SegmentCache::GetSegments(uint64_t periodStart,
                                                          const std::string& periodId,
                                                          uint32_t periodSequence,
                                                          const std::string& adaptationId,
                                                          const std::string& representationId)
{
  std::vector<PLAYLIST::CSegment> segments;
  if (!m_available)
    return segments;

  std::lock_guard lock(m_mutex);
  for (const Entry& entry : m_entries)
  {
    const Key& key = entry.key;
    if (entry.segment && !entry.segment->IsInitialization() && key.periodStart == periodStart &&
        key.periodId == periodId && key.periodSequence == periodSequence &&
        key.adaptationId == adaptationId && key.representationId == representationId)
      segments.push_back(*entry.segment);
  }
  std::sort(segments.begin(), segments.end(),
            [](const auto& a, const auto& b) { return a.startPTS_ < b.startPTS_; });
  return segments;
}

bool SegmentCache::HasPeriod(uint64_t periodStart,
                             const std::string& periodId,
                             uint32_t periodSequence)
{
  if (!m_available)
    return false;

  std::lock_guard lock(m_mutex);
  return std::any_of(m_entries.begin(), m_entries.end(),
                     [&](const Entry& entry)
                     {
                       return entry.segment && !entry.segment->IsInitialization() &&
                              entry.key.periodStart == periodStart &&
                              entry.key.periodId == periodId &&
                              entry.key.periodSequence == periodSequence;
                     });
}

std::vector<std::pair<uint64_t, uint64_t>> SegmentCache::GetCachedRanges() const
{
  std::vector<std::pair<uint64_t, uint64_t>> video;
  std::vector<std::pair<uint64_t, uint64_t>> audio;
  std::lock_guard lock(m_mutex);
  for (const Entry& entry : m_entries)
  {
    if (!entry.segment || entry.segment->IsInitialization() ||
        entry.key.cacheEndUs <= entry.key.cacheStartUs)
      continue;
    (entry.key.isVideo ? video : audio)
        .emplace_back(entry.key.cacheStartUs, entry.key.cacheEndUs);
  }

  auto& ranges = video.empty() ? audio : video;
  std::sort(ranges.begin(), ranges.end());
  size_t out = 0;
  for (const auto& range : ranges)
  {
    if (out && range.first <= ranges[out - 1].second + 100000)
      ranges[out - 1].second = std::max(ranges[out - 1].second, range.second);
    else
      ranges[out++] = range;
  }
  ranges.resize(out);
  return ranges;
}
