// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Perf.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Orca/JitWarm.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Overlay.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace Orca::Perf
{
namespace
{
double Ms(std::chrono::nanoseconds time)
{
  return std::chrono::duration<double, std::milli>(time).count();
}

// One decimal, never NaN or infinite.
std::string Num(double value)
{
  if (!std::isfinite(value))
    value = 0;
  return fmt::format("{:.1f}", std::round(value * 10) / 10);
}

std::string Quote(std::string_view text)
{
  // CleanName already left out quotes, backslashes and control characters.
  return fmt::format("\"{}\"", text);
}
}  // namespace

Mode ModeFor(int peers)
{
  return peers <= 0 ? Mode::Solo : peers == 1 ? Mode::OneVsOne : Mode::Multi;
}

const char* ModeName(Mode mode)
{
  switch (mode)
  {
  case Mode::Solo:
    return "solo";
  case Mode::OneVsOne:
    return "1v1";
  case Mode::Multi:
    return "multi";
  }
  return "solo";
}

double Work(const Frame& frame)
{
  return std::max(0.0, frame.interval_ms - frame.sleep_ms - frame.wait_ms);
}

double ForwardWork(const Frame& frame)
{
  return std::max(0.0, Work(frame) - std::max(0.0, frame.rerun_ms));
}

double Percentile(std::vector<double> values, double p)
{
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  const double rank = std::ceil(std::clamp(p, 0.0, 100.0) / 100 * values.size());
  const std::size_t index =
      static_cast<std::size_t>(std::clamp(rank, 1.0, static_cast<double>(values.size()))) - 1;
  return values[index];
}

u64 SceneCode(std::string_view name)
{
  u64 code = 0;
  unsigned shift = 0;
  for (unsigned char c : name)
  {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      continue;
    code |= u64(c) << shift;
    shift += 8;
    if (shift == 64)
      break;
  }
  return code;
}

std::string SceneCodeName(u64 code)
{
  std::string name;
  for (int i = 0; i < 8 && code; ++i, code >>= 8)
  {
    const char c = static_cast<char>(code & 0xff);
    name += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '?';
  }
  return name.empty() ? "unknown" : name;
}

std::optional<SceneTransition> PaceMeter::Present(std::chrono::nanoseconds now, u64 scene,
                                                  bool headless)
{
  std::optional<SceneTransition> transition;
  if (m_last && headless == m_headless && !headless && now >= *m_last)
  {
    const double ms = Ms(now - *m_last);
    ++m_window.n;
    m_window.p[3] = std::max(m_window.p[3], ms);
    for (std::size_t i = 0; i < m_window.h.size(); ++i)
      m_window.h[i] += ms > std::array{25.0, 33.0, 50.0}[i];
    if (m_size < m_samples.size())
      m_samples[m_size++] = ms;
    else
      ++m_window.dropped;
  }
  if (scene && m_scene_last && scene != m_scene && headless == m_headless)
    transition = SceneTransition{m_scene, scene, Ms(now - *m_scene_last), headless};
  // Unreadable scene frames must not move the last old-scene timestamp during a load.
  if (scene)
  {
    m_scene = scene;
    m_scene_last = now;
  }
  m_last = now;
  m_headless = headless;
  m_window.scene = m_scene;
  m_window.headless = headless;
  return transition;
}

void PaceMeter::Rollback(int depth)
{
  if (depth <= 0)
    return;
  ++m_window.rb[std::min(depth, 4) - 1];
  m_window.rm = std::max(m_window.rm, static_cast<u64>(depth));
}

void PaceMeter::BreakInterval()
{
  m_last.reset();
  m_scene_last.reset();
}

PaceWindow PaceMeter::Take()
{
  std::sort(m_samples.begin(), m_samples.begin() + m_size);
  if (m_size)
  {
    for (std::size_t i = 0; i < 3; ++i)
    {
      const auto rank = static_cast<std::size_t>(std::ceil(std::array{.50, .95, .99}[i] * m_size));
      m_window.p[i] = m_samples[rank - 1];
    }
  }
  const PaceWindow out = m_window;
  m_window = {};
  m_window.scene = m_scene;
  m_window.headless = m_headless;
  m_size = 0;
  return out;
}

namespace
{
// Fixed widths make the complete protocol line (prefix and newline included) fit 300 bytes,
// even after an arbitrarily long stall or a pathological event count.
u64 PaceCount(u64 value)
{
  return std::min<u64>(value, 999999);
}
std::string PaceMs(double value)
{
  return Num(std::isfinite(value) ? std::clamp(value, 0.0, 99999.9) : 0);
}
}  // namespace

std::string PaceJson(const PaceWindow& w)
{
  return fmt::format(
      "{{\"v\":1,\"k\":\"pace\",\"s\":\"{}\",\"n\":{},\"p50\":{},\"p95\":{},\"p99\":{},\"max\":{},"
      "\"h25\":{},\"h33\":{},\"h50\":{},\"rb\":[{},{},{},{}],\"rm\":{},\"sc\":{},\"sm\":{},\"d\":{}"
      ",\"h\":{}}}",
      SceneCodeName(w.scene), PaceCount(w.n), PaceMs(w.p[0]), PaceMs(w.p[1]), PaceMs(w.p[2]),
      PaceMs(w.p[3]), PaceCount(w.h[0]), PaceCount(w.h[1]), PaceCount(w.h[2]), PaceCount(w.rb[0]),
      PaceCount(w.rb[1]), PaceCount(w.rb[2]), PaceCount(w.rb[3]), PaceCount(w.rm), PaceCount(w.sc),
      PaceMs(w.sm), PaceCount(w.dropped), w.headless ? 1 : 0);
}

std::string SceneJson(const SceneTransition& s)
{
  return fmt::format("{{\"v\":1,\"k\":\"scene\",\"from\":\"{}\",\"to\":\"{}\",\"ms\":{},\"h\":{}}}",
                     SceneCodeName(s.from), SceneCodeName(s.to), PaceMs(s.ms), s.headless ? 1 : 0);
}

bool Window::Counts(const Rules& rules) const
{
  return n >= rules.count_frames && mode != Mode::Multi;
}

void WindowMeter::AddValid(double work_ms, double gpu_ms, double rerun_ms, int reruns)
{
  m_work.push_back(work_ms);
  m_gpu.push_back(gpu_ms);
  m_work_ms += work_ms;
  const double rerun = std::clamp(rerun_ms, 0.0, work_ms);
  m_forward_ms += work_ms - rerun;
  m_rerun_ms += rerun;
  m_reruns += std::max(0, reruns);
}

void WindowMeter::AddShown(int frames)
{
  m_shown += std::max(0, frames);
}

void WindowMeter::AddRunning(double ms)
{
  m_running_ms += std::max(0.0, ms);
}

void WindowMeter::AddRerun(int frames)
{
  m_rerun += std::max(0, frames);
}

void WindowMeter::SeePeers(int peers)
{
  m_peers = std::max(m_peers, peers);
}

Window WindowMeter::Take()
{
  Window window;
  window.mode = ModeFor(m_peers);
  window.n = static_cast<int>(m_work.size());
  if (window.n > 0)
  {
    window.cap = m_work_ms > 0 ? std::min(CAP_MAX, window.n / (m_work_ms / 1000)) : CAP_MAX;
    window.fc =
        m_forward_ms > 0 ? std::min(CAP_MAX, window.n / (m_forward_ms / 1000)) : CAP_MAX;
    window.rv = m_reruns;
    window.rw = m_reruns > 0 ? m_rerun_ms / m_reruns : 0;
    window.w50 = Percentile(m_work, 50);
    window.w95 = Percentile(m_work, 95);
    window.g95 = Percentile(m_gpu, 95);
  }
  window.fps = m_running_ms > 0 ? m_shown / (m_running_ms / 1000) : 0;
  window.rbf = m_rerun;
  *this = {};
  return window;
}

std::optional<double> RerunCost(const Window& window, const Rules& rules)
{
  if (window.rv >= rules.rerun_min && window.rw >= 0)
    return window.rw;
  return std::nullopt;
}

double CapAtRef(const Window& window, const Rules& rules, std::optional<double> known_rw)
{
  const double fc = window.fc;
  if (!(fc > 0))
    return window.cap;
  std::optional<double> rw = RerunCost(window, rules);
  if (!rw)
    rw = known_rw;
  if (!rw)
    return fc;
  return std::min(fc, 1000 / (1000 / fc + rules.rerun_ref * *rw));
}

bool AutoSwitch::Feed(const Window& window)
{
  if (m_fired || !window.Counts(m_rules))
    return false;
  if (const std::optional<double> rw = RerunCost(window, m_rules))
    m_rerun_cost = rw;
  const double cap = CapAtRef(window, m_rules, m_rerun_cost);
  if (window.pf || cap >= m_rules.auto_cap)
  {
    m_run.clear();
    return false;
  }
  m_run.push_back(cap);
  if (static_cast<int>(m_run.size()) > m_rules.auto_windows)
    m_run.erase(m_run.begin());
  if (static_cast<int>(m_run.size()) < m_rules.auto_windows)
    return false;
  m_fired = true;
  std::vector<double> caps = m_run;
  std::sort(caps.begin(), caps.end());
  const std::size_t mid = caps.size() / 2;
  m_trigger_cap = caps.size() % 2 ? caps[mid] : (caps[mid - 1] + caps[mid]) / 2;
  m_run.clear();
  return true;
}

std::string CleanName(std::string_view name)
{
  std::string out;
  for (const char c : name)
  {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u >= 0x7f || c == '"' || c == '\\')
      continue;
    // One space between words.
    if (c == ' ' && (out.empty() || out.back() == ' '))
      continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (out.size() > NAME_CHARS)
  {
    out.resize(NAME_CHARS);
    while (!out.empty() && out.back() == ' ')
      out.pop_back();
  }
  return out;
}

std::string InfoJson(const Info& info)
{
  const auto build = [&info](bool with_cpu, std::size_t adapters) {
    std::string ad;
    for (std::size_t i = 0; i < std::min(adapters, info.adapters.size()); ++i)
      ad += fmt::format("{}{}", i ? "," : "", Quote(CleanName(info.adapters[i])));
    std::string json =
        fmt::format("{{\"v\":1,\"k\":\"info\",\"be\":{},\"gpu\":{}{},\"ad\":[{}],\"ai\":{}",
                    Quote(CleanName(info.backend)), Quote(CleanName(info.gpu)),
                    info.driver.empty() ? std::string() :
                                          fmt::format(",\"drv\":{}", Quote(CleanName(info.driver))),
                    ad, info.adapter);
    if (with_cpu)
      json += fmt::format(",\"cpu\":{}", Quote(CleanName(info.cpu)));
    json += fmt::format(",\"th\":{},\"os\":{},\"rev\":{}}}", info.threads,
                        Quote(CleanName(info.os)), Quote(CleanName(info.rev)));
    return json;
  };
  const std::size_t room = LINE_BYTES - std::string_view("orca perf ").size();
  std::string json = build(true, 3);
  if (json.size() > room)
    json = build(false, 3);
  if (json.size() > room)
    json = build(false, 1);
  return json;
}

std::string WindowJson(const Window& window)
{
  std::string json = fmt::format(
      "{{\"v\":1,\"k\":\"win\",\"m\":\"{}\",\"n\":{},\"cap\":{},\"fc\":{},\"rw\":{},\"rv\":{},"
      "\"w50\":{},\"w95\":{},\"g95\":{},\"fps\":{},\"rbf\":{},\"res\":{},\"pf\":{},\"as\":{}",
      ModeName(window.mode), window.n, Num(window.cap), Num(window.fc),
      Num(std::min(window.rw, 9999.9)), std::min(window.rv, 999'999), Num(window.w50),
      Num(window.w95), Num(window.g95), Num(window.fps), window.rbf, window.res, window.pf ? 1 : 0,
      window.as ? 1 : 0);
  if (window.test)
    json += ",\"t\":1";
  return json + "}";
}

std::string AutoJson(double cap)
{
  return fmt::format("{{\"v\":1,\"k\":\"auto\",\"cap\":{}}}", Num(cap));
}

namespace
{
struct GfxSpec
{
  std::string_view name;
  GfxKey key;
  int min;
  int max;
};
constexpr std::array<GfxSpec, 7> GFX_SPECS{{
    {"res", GfxKey::Res, 0, 4},
    {"aniso", GfxKey::Aniso, -1, 4},
    {"filter", GfxKey::Filter, 0, 2},
    {"resample", GfxKey::Resample, 0, 6},
    {"efb", GfxKey::Efb, 0, 1},
    {"gpudecode", GfxKey::GpuDecode, 0, 1},
    {"cpucull", GfxKey::CpuCull, 0, 1},
}};

// A whole number with an optional minus sign, nothing else.
std::optional<int> ParseInt(std::string_view text)
{
  if (text.empty() || text.size() > 3)
    return std::nullopt;
  const bool negative = text.front() == '-';
  if (negative)
    text.remove_prefix(1);
  if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  int value = 0;
  for (const char c : text)
    value = value * 10 + (c - '0');
  return negative ? -value : value;
}
}  // namespace

std::optional<GfxChange> ParseGfx(std::string_view line)
{
  std::istringstream words{std::string(line)};
  std::string cmd, name, value, more;
  if (!(words >> cmd >> name >> value) || (words >> more) || cmd != "gfx")
    return std::nullopt;
  const auto spec = std::ranges::find(GFX_SPECS, name, &GfxSpec::name);
  if (spec == GFX_SPECS.end())
    return std::nullopt;
  const std::optional<int> number = ParseInt(value);
  if (!number || *number < spec->min || *number > spec->max)
    return std::nullopt;
  return GfxChange{spec->key, *number};
}

Config::Location GfxLocation(GfxKey key)
{
  switch (key)
  {
  case GfxKey::Res:
    return Config::GFX_EFB_SCALE.GetLocation();
  case GfxKey::Aniso:
    return Config::GFX_ENHANCE_MAX_ANISOTROPY.GetLocation();
  case GfxKey::Filter:
    return Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING.GetLocation();
  case GfxKey::Resample:
    return Config::GFX_ENHANCE_OUTPUT_RESAMPLING.GetLocation();
  case GfxKey::Efb:
    return Config::GFX_HACK_COPY_EFB_SCALED.GetLocation();
  case GfxKey::GpuDecode:
    return Config::GFX_ENABLE_GPU_TEXTURE_DECODING.GetLocation();
  case GfxKey::CpuCull:
    return Config::GFX_CPU_CULL.GetLocation();
  }
  return Config::GFX_EFB_SCALE.GetLocation();
}

std::vector<Config::Location> GfxLocations()
{
  std::vector<Config::Location> locations;
  for (const GfxSpec& spec : GFX_SPECS)
    locations.push_back(GfxLocation(spec.key));
  return locations;
}

std::array<GfxChange, 5> PerformancePreset()
{
  return {{{GfxKey::Res, 1},
           {GfxKey::Efb, 0},
           {GfxKey::Aniso, static_cast<int>(AnisotropicFilteringMode::Default)},
           {GfxKey::Filter, static_cast<int>(TextureFilteringMode::Default)},
           {GfxKey::Resample, static_cast<int>(OutputResamplingMode::Default)}}};
}

void ApplyGfx(const GfxChange& change)
{
  switch (change.key)
  {
  case GfxKey::Res:
    Config::SetCurrent(Config::GFX_EFB_SCALE, change.value);
    break;
  case GfxKey::Aniso:
    Config::SetCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                       static_cast<AnisotropicFilteringMode>(change.value));
    break;
  case GfxKey::Filter:
    Config::SetCurrent(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                       static_cast<TextureFilteringMode>(change.value));
    break;
  case GfxKey::Resample:
    Config::SetCurrent(Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
                       static_cast<OutputResamplingMode>(change.value));
    break;
  case GfxKey::Efb:
    Config::SetCurrent(Config::GFX_HACK_COPY_EFB_SCALED, change.value != 0);
    break;
  case GfxKey::GpuDecode:
    Config::SetCurrent(Config::GFX_ENABLE_GPU_TEXTURE_DECODING, change.value != 0);
    break;
  case GfxKey::CpuCull:
    Config::SetCurrent(Config::GFX_CPU_CULL, change.value != 0);
    break;
  }
}

bool PerformancePicture()
{
  return Config::Get(Config::GFX_EFB_SCALE) == 1 &&
         !Config::Get(Config::GFX_HACK_COPY_EFB_SCALED) &&
         Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY) == AnisotropicFilteringMode::Default &&
         Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING) ==
             TextureFilteringMode::Default &&
         Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING) == OutputResamplingMode::Default;
}

bool PerformanceNow()
{
  return PerformancePicture() && Config::Get(Config::GFX_MSAA) <= 1 &&
         !Config::Get(Config::GFX_SSAA);
}

void Meter::NoteScene(std::optional<bool> in_match)
{
  if (!in_match)
    return;
  m_scene_match = *in_match;
  if (!*in_match)
    m_match_frames = 0;
}

std::optional<Window> Meter::OnBoundary(const Boundary& boundary, std::chrono::nanoseconds now,
                                        const Totals& totals)
{
  if (!boundary.first_run)
  {
    // A re-run's time belongs to the next forward frame; a catch-up's makes it meaningless.
    if (boundary.excluded)
      m_spoiled = true;
    // Where the re-runs end: the forward frame's own work starts here.
    m_last_rerun = now;
    m_rerun_totals = totals;
    ++m_reruns;
    return std::nullopt;
  }
  if (m_scene_match)
    ++m_match_frames;
  else
    m_match_frames = 0;

  const bool has_last = m_last_forward.has_value();
  Frame frame;
  if (has_last)
  {
    frame.interval_ms = Ms(now - *m_last_forward);
    frame.sleep_ms = Ms(totals.sleep - m_totals.sleep);
    frame.wait_ms = Ms(totals.wait - m_totals.wait);
    frame.gpu_ms = Ms(totals.present - m_totals.present);
    if (m_last_rerun && *m_last_rerun >= *m_last_forward)
    {
      // From the last forward boundary to the last re-run one, waits out.
      frame.rerun_ms = Ms(*m_last_rerun - *m_last_forward) -
                       Ms(m_rerun_totals.sleep - m_totals.sleep) -
                       Ms(m_rerun_totals.wait - m_totals.wait);
      frame.reruns = m_reruns;
    }
  }
  const std::chrono::nanoseconds paused = boundary.paused_total - m_paused;
  // A new session restarts its count.
  const u64 rerun = boundary.rerun_total >= m_rerun_total ? boundary.rerun_total - m_rerun_total :
                                                            boundary.rerun_total;
  const bool valid = has_last && !m_spoiled && !boundary.excluded && !boundary.hidden &&
                     !boundary.held && paused.count() <= 0 && m_match_frames > m_rules.warm_frames;

  if (has_last)
    m_window.AddRunning(frame.interval_ms - Ms(paused));
  if (!boundary.excluded)
    m_window.AddShown(1 + boundary.held_frames);
  m_window.AddRerun(static_cast<int>(std::min<u64>(rerun, 1'000'000)));
  m_window.SeePeers(boundary.peers);
  if (valid)
    m_window.AddValid(Work(frame), frame.gpu_ms, frame.rerun_ms, frame.reruns);

  m_last_forward = now;
  m_totals = totals;
  m_last_rerun.reset();
  m_reruns = 0;
  m_paused = boundary.paused_total;
  m_rerun_total = boundary.rerun_total;
  m_spoiled = false;

  if (!m_window.Due(m_rules))
    return std::nullopt;
  return m_window.Take();
}

// ---- At run time ----

namespace
{
std::atomic<bool> s_ready{false};
std::atomic<s64> s_sleep_ns{0};
std::atomic<s64> s_present_ns{0};
std::atomic<s64> s_wait_ns{0};
std::atomic<int> s_step_scopes{0};
std::mutex s_gfx_mutex;
struct PresentEvent
{
  std::chrono::nanoseconds time;
  u64 scene;
  bool headless;
};
// Single graphics producer, CPU consumer. Release/acquire publishes complete event slots.
constexpr std::size_t PRESENT_QUEUE_SIZE = 4096;
std::array<PresentEvent, PRESENT_QUEUE_SIZE> s_presents;
std::atomic<u64> s_present_write{0};
std::atomic<u64> s_present_read{0};
std::atomic<u64> s_present_dropped{0};
std::atomic<u64> s_pace_scene{0};
std::atomic<u64> s_compiles{0};
std::atomic<s64> s_compile_ns{0};
static_assert(std::atomic<u64>::is_always_lock_free);
// The last value queued for each key (GfxKey order), applied at the next forward boundary.
std::array<std::optional<int>, 7> s_gfx_queue;

// CPU thread only.
struct Run
{
  bool info_sent = false;
  std::optional<Meter> meter;
  std::optional<AutoSwitch> auto_switch;
  PaceMeter pace;
  std::optional<JitWarm::Scene> scene;
  std::chrono::nanoseconds paused{};
};
Run s_run;

// A harness run (ORCA_TEST_COMMANDS=1, which the app never passes) in a session.
bool TestRun()
{
  static const bool test = GetEnv("ORCA_TEST_COMMANDS") == "1";
  return test && SessionActive();
}

struct TestSlow
{
  double ms = 0;
  double performance_ms = 0;
};
const TestSlow& ReadTestSlow()
{
  static const TestSlow slow = [] {
    TestSlow out;
    const std::string value = GetEnv("ORCA_TEST_SLOW");
    if (!TestRun() || value.empty())
      return out;
    double ms = 0, performance_ms = 0;
    const int read = std::sscanf(value.c_str(), "%lf,%lf", &ms, &performance_ms);
    if (read >= 1)
      out.ms = std::clamp(ms, 0.0, 100.0);
    if (read >= 2)
      out.performance_ms = std::clamp(performance_ms, 0.0, 100.0);
    return out;
  }();
  return slow;
}

// Spins the CPU thread (busy, not asleep: it is work, as a slow machine's would be).
void Spin(double ms)
{
  if (ms <= 0)
    return;
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<double, std::milli>(ms));
  while (std::chrono::steady_clock::now() < until)
  {
  }
}

Rules ReadRules()
{
  Rules rules;
  const std::string test = GetEnv("ORCA_TEST_PERF");
  if (TestRun() && !test.empty())
  {
    double window_s = rules.window_s;
    int warm = rules.warm_frames;
    if (std::sscanf(test.c_str(), "%lf,%d", &window_s, &warm) >= 1)
    {
      window_s = std::clamp(window_s, 1.0, 60.0);
      rules.count_frames =
          static_cast<int>(std::lround(rules.count_frames * window_s / rules.window_s));
      rules.window_s = window_s;
      rules.warm_frames = std::clamp(warm, 0, 3600);
    }
  }
  return rules;
}

bool AutoAllowed()
{
  static const bool allowed = !(TestRun() && GetEnv("ORCA_PERF_AUTO") == "0");
  return allowed;
}

std::string OsName()
{
#if defined(_WIN32)
  return "win";
#elif defined(__APPLE__)
  return "mac";
#elif defined(__linux__)
  return "linux";
#else
  return "other";
#endif
}

void Print(const std::string& json)
{
  Status::Line("orca perf " + json);
}

void SendInfo()
{
  Info info;
  info.backend =
      g_video_backend ? g_video_backend->GetConfigName() : Config::Get(Config::MAIN_GFX_BACKEND);
  info.adapters = g_backend_info.Adapters;
  info.adapter = Config::Get(Config::GFX_ADAPTER);
  info.driver = g_backend_info.DriverVersion;
  if (!g_backend_info.AdapterName.empty())
    info.gpu = g_backend_info.AdapterName;
  else if (info.adapter >= 0 && info.adapter < static_cast<int>(info.adapters.size()))
    info.gpu = info.adapters[info.adapter];
  else if (!info.adapters.empty())
    info.gpu = info.adapters.front();
  info.cpu = cpu_info.model_name;
  info.threads = static_cast<int>(std::thread::hardware_concurrency());
  info.os = OsName();
  info.rev = Common::GetScmRevGitStr().substr(0, 10);
  const std::string json = InfoJson(info);
  NOTICE_LOG_FMT(ROLLBACK, "Orca perf: {}; EFB scale in use {}", json,
                 g_framebuffer_manager ? g_framebuffer_manager->GetEFBScale() : 0);
  Print(json);
}

void ApplyQueuedGfx()
{
  std::vector<GfxChange> changes;
  {
    std::lock_guard lock(s_gfx_mutex);
    for (std::size_t key = 0; key < s_gfx_queue.size(); ++key)
    {
      if (s_gfx_queue[key])
        changes.push_back({static_cast<GfxKey>(key), *s_gfx_queue[key]});
      s_gfx_queue[key].reset();
    }
  }
  if (changes.empty())
    return;
  for (const GfxChange& change : changes)
    ApplyGfx(change);
  // The player chose new settings: give them their own windows before switching.
  if (s_run.auto_switch)
    s_run.auto_switch->Restart();
  NOTICE_LOG_FMT(ROLLBACK, "Orca perf: {} graphics change(s) from the app; {}", changes.size(),
                 DescribeGraphics());
}

void SwitchToPerformance(double cap)
{
  for (const GfxChange& change : PerformancePreset())
    ApplyGfx(change);
  // A post-processing shader a GFX.ini set is not part of any preset.
  if (!Config::Get(Config::GFX_ENHANCE_POST_SHADER).empty())
    Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, std::string());
  UX::ShowToast("Graphics set to Performance to hold 60 fps");
  NOTICE_LOG_FMT(ROLLBACK,
                 "Orca perf: this machine ran at {:.1f} fps: graphics set to Performance; {}", cap,
                 DescribeGraphics());
  Print(AutoJson(cap));
}

void FinishWindow(Window window)
{
  window.res = g_framebuffer_manager ? static_cast<int>(g_framebuffer_manager->GetEFBScale()) :
                                       Config::Get(Config::GFX_EFB_SCALE);
  window.pf = PerformanceNow();
  window.as = s_run.auto_switch && s_run.auto_switch->Fired() && PerformancePicture();
  window.test = TestRulesActive();
  Print(WindowJson(window));
  PaceWindow pace = s_run.pace.Take();
  pace.dropped += s_present_dropped.exchange(0, std::memory_order_relaxed);
  pace.sc = s_compiles.exchange(0, std::memory_order_relaxed);
  pace.sm = Ms(std::chrono::nanoseconds(s_compile_ns.exchange(0, std::memory_order_relaxed)));
  Print(PaceJson(pace));
  if (s_run.auto_switch && AutoAllowed() && s_run.auto_switch->Feed(window))
    SwitchToPerformance(s_run.auto_switch->TriggerCap());
}
}  // namespace

void Ready()
{
  s_ready = true;
}

bool TestRulesActive()
{
  static const bool active =
      TestRun() && (!GetEnv("ORCA_TEST_PERF").empty() || GetEnv("ORCA_PERF_AUTO") == "0" ||
                    !GetEnv("ORCA_TEST_SLOW").empty());
  return active;
}

bool Measuring()
{
  return s_run.meter.has_value();
}

void NewBoot()
{
  s_run.meter.reset();
  s_run.auto_switch.reset();
  s_run.pace = {};
  s_run.scene.reset();
  s_run.paused = {};
  s_pace_scene.store(0, std::memory_order_relaxed);
  s_present_read.store(s_present_write.load(std::memory_order_acquire), std::memory_order_release);
  s_present_dropped.store(0, std::memory_order_relaxed);
  s_compiles.store(0, std::memory_order_relaxed);
  s_compile_ns.store(0, std::memory_order_relaxed);
}

const Rules& ActiveRules()
{
  static const Rules rules = ReadRules();
  return rules;
}

void AddSleep(std::chrono::nanoseconds time)
{
  if (s_step_scopes.load(std::memory_order_relaxed) == 0)
    s_sleep_ns.fetch_add(time.count(), std::memory_order_relaxed);
}

void AddPresent(std::chrono::nanoseconds time)
{
  if (s_step_scopes.load(std::memory_order_relaxed) == 0)
    s_present_ns.fetch_add(time.count(), std::memory_order_relaxed);
}

void Presented(bool headless)
{
  if (!s_ready.load(std::memory_order_relaxed))
    return;
  const u64 write = s_present_write.load(std::memory_order_relaxed);
  if (write - s_present_read.load(std::memory_order_acquire) >= PRESENT_QUEUE_SIZE)
  {
    s_present_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s_presents[write % PRESENT_QUEUE_SIZE] = {
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()),
      s_pace_scene.load(std::memory_order_relaxed), headless};
  s_present_write.store(write + 1, std::memory_order_release);
}

void NotePaceScene(const Core::CPUThreadGuard& guard)
{
  const auto scene = JitWarm::CurrentScene(guard);
  if (scene == s_run.scene)
    return;
  s_run.scene = scene;
  s_pace_scene.store(scene ? SceneCode(JitWarm::SceneName(guard, scene->name)) : 0,
                     std::memory_order_relaxed);
}

void NoteRollback(int depth)
{
  if (s_ready.load(std::memory_order_relaxed))
    s_run.pace.Rollback(depth);
}

CompileScope::CompileScope()
{
  if (s_ready.load(std::memory_order_relaxed))
    m_start = std::chrono::steady_clock::now();
}

CompileScope::~CompileScope()
{
  if (!m_start)
    return;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - *m_start);
  s_compile_ns.fetch_add(ns.count(), std::memory_order_relaxed);
  s_compiles.fetch_add(1, std::memory_order_relaxed);
}

void AddWait(std::chrono::nanoseconds time)
{
  s_wait_ns.fetch_add(time.count(), std::memory_order_relaxed);
}

StepScope::StepScope()
{
  s_step_scopes.fetch_add(1, std::memory_order_relaxed);
}

StepScope::~StepScope()
{
  s_step_scopes.fetch_sub(1, std::memory_order_relaxed);
}

void NoteScene(std::optional<bool> in_match)
{
  if (s_run.meter)
    s_run.meter->NoteScene(in_match);
}

void QueueGfx(const GfxChange& change)
{
  const auto key = static_cast<std::size_t>(change.key);
  if (key >= s_gfx_queue.size())
    return;
  std::lock_guard lock(s_gfx_mutex);
  s_gfx_queue[key] = change.value;
}

void OnBoundary(const Boundary& boundary)
{
  if (boundary.first_run)
    ApplyQueuedGfx();
  if (!s_ready.load(std::memory_order_relaxed))
    return;
  if (!s_run.meter)
  {
    s_run.meter.emplace(ActiveRules());
    s_run.auto_switch.emplace(ActiveRules());
  }
  if (boundary.first_run && !s_run.info_sent)
  {
    s_run.info_sent = true;
    SendInfo();
  }
  const bool paused = boundary.paused_total != s_run.paused;
  if (paused)
  {
    s_run.pace.BreakInterval();
    s_run.paused = boundary.paused_total;
  }
  u64 read = s_present_read.load(std::memory_order_relaxed);
  const u64 write = s_present_write.load(std::memory_order_acquire);
  if (paused)
    read = write;  // Discard redraws queued while the CPU was paused.
  while (read != write)
  {
    const PresentEvent& event = s_presents[read % PRESENT_QUEUE_SIZE];
    if (auto scene = s_run.pace.Present(event.time, event.scene, event.headless))
      Print(SceneJson(*scene));
    ++read;
  }
  s_present_read.store(read, std::memory_order_release);
  const Meter::Totals totals{std::chrono::nanoseconds(s_sleep_ns.load(std::memory_order_relaxed)),
                             std::chrono::nanoseconds(s_present_ns.load(std::memory_order_relaxed)),
                             std::chrono::nanoseconds(s_wait_ns.load(std::memory_order_relaxed))};
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  if (std::optional<Window> window = s_run.meter->OnBoundary(
          boundary, std::chrono::duration_cast<std::chrono::nanoseconds>(now), totals))
  {
    FinishWindow(*window);
  }
  // A test's slow machine: after the clock was read, so the next forward frame carries it.
  if (boundary.first_run && !boundary.excluded)
  {
    const TestSlow& slow = ReadTestSlow();
    Spin(PerformancePicture() ? slow.performance_ms : slow.ms);
  }
}
}  // namespace Orca::Perf
