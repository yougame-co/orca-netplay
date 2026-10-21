// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Loopback.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

#include "Common/FileUtil.h"

namespace Rollback::Loopback
{
namespace
{
std::string PadHex(const Orca::Net::Pad& pad)
{
  std::string hex;
  for (const u8 byte : pad)
    hex += fmt::format("{:02x}", byte);
  return hex;
}

// Parses the whole field as hex; false on any error (the build has exceptions off).
template <typename T>
bool ParseHex(std::string_view text, T* value)
{
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value, 16);
  return error == std::errc{} && end == text.data() + text.size() && !text.empty();
}

bool ParsePad(std::string_view hex, Orca::Net::Pad* pad)
{
  if (hex.size() != pad->size() * 2)
    return false;
  for (std::size_t i = 0; i < pad->size(); ++i)
  {
    if (!ParseHex(hex.substr(i * 2, 2), &(*pad)[i]))
      return false;
  }
  return true;
}
}  // namespace

std::string FormatRecordedFrame(int frame, const Orca::Net::Pads& pads, u64 checksum)
{
  return fmt::format("{} {} {} {} {} {:016x}\n", frame, PadHex(pads[0]), PadHex(pads[1]),
                     PadHex(pads[2]), PadHex(pads[3]), checksum);
}

std::vector<RecordedFrame> LoadRecording(const std::string& path)
{
  std::ifstream in;
  File::OpenFStream(in, path, std::ios_base::in);
  std::map<int, RecordedFrame> frames;
  std::string line;
  while (std::getline(in, line))
  {
    std::istringstream fields(line);
    int frame = -1;
    std::array<std::string, Orca::Net::MAX_SEATS> pads;
    std::string checksum;
    if (!(fields >> frame >> pads[0] >> pads[1] >> pads[2] >> pads[3] >> checksum) || frame < 0)
      continue;
    RecordedFrame& recorded = frames[frame];
    for (std::size_t port = 0; port < pads.size(); ++port)
    {
      if (!ParsePad(pads[port], &recorded.pads[port]))
        return {};
    }
    if (checksum.size() != 16 || !ParseHex(checksum, &recorded.checksum))
      return {};
  }
  std::vector<RecordedFrame> recording;
  for (const auto& [frame, recorded] : frames)
  {
    if (frame != static_cast<int>(recording.size()))
      return {};
    recording.push_back(recorded);
  }
  return recording;
}

VirtualRemote::VirtualRemote(const std::vector<RecordedFrame>& recording, std::vector<int> seats,
                             int local_seat, int lag, int jitter, int input_delay,
                             int checksum_every)
    : m_recording(recording), m_local_seat(local_seat), m_lag(lag), m_jitter(jitter),
      m_input_delay(input_delay), m_checksum_every(checksum_every)
{
  for (const int seat : seats)
    m_seats.push_back(Seat{seat});
}

bool VirtualRemote::Exhausted() const
{
  const int last = static_cast<int>(m_recording.size()) - 1;
  return std::ranges::all_of(m_seats, [last](const Seat& seat) { return seat.delivered >= last; });
}

int VirtualRemote::Lag(int seat) const
{
  if (m_jitter <= 0)
    return m_lag;
  u32 x = static_cast<u32>(m_ticks) * 2654435761u + static_cast<u32>(seat) * 0x9E3779B9u;
  x ^= x >> 13;
  return m_lag + static_cast<int>(x % static_cast<u32>(m_jitter + 1));
}

void VirtualRemote::Tick()
{
  m_last_tick = std::chrono::steady_clock::now();
  if (m_lead > 0)
    --m_lead;
  else
    ++m_ticks;
}

void VirtualRemote::TickWhileWaiting()
{
  if (m_spike_period.count() <= 0)
  {
    ++m_ticks;
    return;
  }
  // The remote runs at frame rate, but no further ahead than its rollback window allows.
  constexpr std::chrono::microseconds FRAME{16'683};
  const auto now = std::chrono::steady_clock::now();
  if (now - m_last_tick < FRAME || m_lead >= m_max_lead)
    return;
  m_last_tick = now;
  ++m_ticks;
  ++m_lead;
}

void VirtualRemote::StartSpikes(std::chrono::milliseconds period, std::chrono::milliseconds length,
                                int max_lead)
{
  m_spike_period = period;
  m_spike_length = length;
  m_spike_start = std::chrono::steady_clock::now();
  m_last_tick = m_spike_start;
  m_max_lead = max_lead;
}

void VirtualRemote::Send(const Orca::Net::Packet& packet)
{
  if (!packet.pads.empty())
  {
    m_local_newest =
        std::max(m_local_newest, packet.first_frame + static_cast<int>(packet.pads.size()) - 1);
  }
}

std::vector<Orca::Net::Packet> VirtualRemote::Receive()
{
  std::vector<Orca::Net::Packet> packets;
  if (m_spike_period.count() > 0)
  {
    // The first spike starts one period in, and each lasts `length` of every period.
    const auto since = std::chrono::steady_clock::now() - m_spike_start;
    const bool held = since >= m_spike_period && since % m_spike_period < m_spike_length;
    if (held && !m_spiking)
      ++m_spikes;
    m_spiking = held;
    if (held)
      return packets;
  }
  for (Seat& seat : m_seats)
  {
    const int position = m_ticks - Lag(seat.seat);  // the frame this remote player is running
    int newest = std::min(position + m_input_delay, static_cast<int>(m_recording.size()) - 1);
    if (m_silent_from >= 0)
      newest = std::min(newest, m_silent_from - 1);
    while (seat.delivered < newest)
    {
      Orca::Net::Packet packet;
      packet.seat = seat.seat;
      packet.first_frame = seat.delivered + 1;
      const int last = std::min(newest, seat.delivered + 16);
      for (int frame = packet.first_frame; frame <= last; ++frame)
        packet.pads.push_back(m_recording[frame].pads[seat.seat]);
      packet.ack[m_local_seat] = m_local_newest;
      packet.current_frame = std::max(position, 0);
      const int checksum_frame = seat.last_checksum + m_checksum_every;
      if (checksum_frame <= std::min(position, newest) &&
          checksum_frame < static_cast<int>(m_recording.size()))
      {
        packet.checksum_frame = checksum_frame;
        packet.checksum = m_recording[checksum_frame].checksum;
        seat.last_checksum = checksum_frame;
      }
      seat.delivered = last;
      packets.push_back(std::move(packet));
    }
  }
  return packets;
}
}  // namespace Rollback::Loopback
