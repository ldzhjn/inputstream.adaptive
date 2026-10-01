/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ADP
{
// Complete media segments only. The download worker owns this cache, so it needs no lock.
class SegmentMemoryCache
{
public:
  struct Key
  {
    std::string url;
    std::map<std::string, std::string> headers;
    uint64_t number;
    uint64_t startPts;

    bool operator==(const Key& other) const
    {
      return url == other.url && headers == other.headers && number == other.number &&
             startPts == other.startPts;
    }
  };

  explicit SegmentMemoryCache(size_t maxBytes) : m_maxBytes(maxBytes) {}

  bool Get(const Key& key, std::vector<uint8_t>& data)
  {
    auto it = Find(key);
    if (it == m_entries.end())
      return false;

    data = it->data;
    m_entries.splice(m_entries.begin(), m_entries, it);
    return true;
  }

  void Put(Key key, std::vector<uint8_t> data)
  {
    if (data.empty() || data.size() > m_maxBytes)
      return;

    auto old = Find(key);
    if (old != m_entries.end())
    {
      m_sizeBytes -= old->data.size();
      m_entries.erase(old);
    }

    while (m_sizeBytes + data.size() > m_maxBytes)
    {
      m_sizeBytes -= m_entries.back().data.size();
      m_entries.pop_back();
    }

    m_sizeBytes += data.size();
    m_entries.push_front({std::move(key), std::move(data)});
  }

private:
  struct Entry
  {
    Key key;
    std::vector<uint8_t> data;
  };

  std::list<Entry>::iterator Find(const Key& key)
  {
    return std::find_if(m_entries.begin(), m_entries.end(),
                        [&key](const Entry& entry) { return entry.key == key; });
  }

  size_t m_maxBytes;
  size_t m_sizeBytes{0};
  std::list<Entry> m_entries;
};
} // namespace ADP
