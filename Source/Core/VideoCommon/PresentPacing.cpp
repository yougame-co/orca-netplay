// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PresentPacing.h"

#include <algorithm>

namespace VideoCommon
{
PresentPacer::PresentPacer() : PresentPacer(Tuning{})
{
}

PresentPacer::PresentPacer(const Tuning& tuning) : m_tuning(tuning)
{
  m_tuning.window = std::max<std::size_t>(m_tuning.window, 1);
  m_tuning.quantile_permille = std::clamp(m_tuning.quantile_permille, 0, 1000);
  m_arrivals.reserve(m_tuning.window);
  m_scratch.reserve(m_tuning.window);
}

PresentPacer::Duration PresentPacer::Next(Duration arrival)
{
  arrival = std::clamp(arrival, m_tuning.earliest, m_tuning.latest);
  const bool first = m_arrivals.empty();
  if (m_arrivals.size() < m_tuning.window)
    m_arrivals.push_back(arrival);
  else
    m_arrivals[m_next] = arrival;
  m_next = (m_next + 1) % m_tuning.window;

  m_scratch.assign(m_arrivals.begin(), m_arrivals.end());
  const std::size_t rank = (m_scratch.size() - 1) * m_tuning.quantile_permille / 1000;
  std::nth_element(m_scratch.begin(), m_scratch.begin() + rank, m_scratch.end());
  const Duration wanted = m_scratch[rank];

  if (first)
    m_offset = wanted;
  else if (wanted > m_offset)
    m_offset = std::min(wanted, m_offset + m_tuning.rise_per_frame);
  else
    m_offset = std::max(wanted, m_offset - m_tuning.fall_per_frame);
  return m_offset;
}
}  // namespace VideoCommon
