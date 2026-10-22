// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MsgHandler.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fmt/format.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#ifdef _WIN32
#include "Common/StringUtil.h"
#endif

namespace Common
{
namespace
{
// Default non library dependent panic alert
bool DefaultMsgHandler(const char* caption, const char* text, bool yes_no, MsgType style)
{
#ifdef _WIN32
  int window_style = MB_ICONINFORMATION;
  if (style == MsgType::Question)
    window_style = MB_ICONQUESTION;
  if (style == MsgType::Warning)
    window_style = MB_ICONWARNING;

  return IDYES == MessageBox(0, UTF8ToTStr(text).c_str(), UTF8ToTStr(caption).c_str(),
                             window_style | (yes_no ? MB_YESNO : MB_OK));
#else
  fmt::print(stderr, "{}\n", text);

  // Return no to any question (which will in general crash the emulator)
  return false;
#endif
}

// Default (non) translator
std::string DefaultStringTranslator(const char* text)
{
  return text;
}

MsgAlertHandler s_msg_handler = DefaultMsgHandler;
PanicAlertLogHandler s_panic_log_handler = nullptr;
StringTranslator s_str_translator = DefaultStringTranslator;
bool s_alert_enabled = true;
bool s_abort_on_panic_alert = false;

std::string SourceRelativePath(std::string path)
{
  std::replace(path.begin(), path.end(), '\\', '/');
  const size_t source = path.rfind("/Source/");
  if (source != std::string::npos)
    return path.substr(source + 8);
  if (path.starts_with("Source/"))
    return path.substr(7);
  // A compiler may already supply a relative path. For an absolute path outside Source, keep
  // only the filename rather than disclosing the build machine's directories.
  if (path.starts_with('/') || (path.size() > 1 && path[1] == ':'))
    return path.substr(path.find_last_of('/') + 1);
  return path;
}

std::string SingleLinePanic(std::string text, const std::string& file, int line)
{
  // ASSERT and ASSERT_MSG include __FILE__ in their text as well as in the log callsite.
  constexpr std::string_view file_label = "\n  File: ";
  const size_t label = text.rfind(file_label);
  if (label != std::string::npos)
  {
    const size_t start = label + file_label.size();
    const size_t end = text.find_first_of("\r\n", start);
    text.replace(start, end - start, SourceRelativePath(text.substr(start, end - start)));
  }

  std::string result = fmt::format("orca error assert at {}:{} | ", file, line);
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '\r' || text[i] == '\n')
    {
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
        ++i;
      result += " | ";
    }
    else
    {
      result += text[i];
    }
  }
  return result;
}

const char* GetCaption(MsgType style)
{
  static const std::string info_caption = GetStringT("Information");
  static const std::string ques_caption = GetStringT("Question");
  static const std::string warn_caption = GetStringT("Warning");
  static const std::string crit_caption = GetStringT("Critical");

  switch (style)
  {
  case MsgType::Information:
    return info_caption.c_str();
  case MsgType::Question:
    return ques_caption.c_str();
  case MsgType::Warning:
    return warn_caption.c_str();
  case MsgType::Critical:
    return crit_caption.c_str();
  default:
    return "Unhandled caption";
  }
}

Log::LogLevel GetMessageAlertLogLevel(const MsgType style)
{
  switch (style)
  {
  case MsgType::Critical:
    return Log::LogLevel::LERROR;
  case MsgType::Warning:
    return Log::LogLevel::LWARNING;
  default:
    return Log::LogLevel::LINFO;
  }
}
}  // Anonymous namespace

// Select which of these functions that are used for message boxes. If
// Qt is enabled we will use QtMsgAlertHandler() that is defined in Main.cpp
MsgAlertHandler RegisterMsgAlertHandler(MsgAlertHandler handler)
{
  return std::exchange(s_msg_handler, handler);
}

PanicAlertLogHandler RegisterPanicAlertLogHandler(PanicAlertLogHandler handler)
{
  return std::exchange(s_panic_log_handler, handler);
}

// Select translation function.
void RegisterStringTranslator(StringTranslator translator)
{
  s_str_translator = translator;
}

// enable/disable the alert handler
void SetEnableAlert(bool enable)
{
  s_alert_enabled = enable;
}

void SetAbortOnPanicAlert(bool should_abort)
{
  s_abort_on_panic_alert = should_abort;
}

std::string GetStringT(const char* string)
{
  return s_str_translator(string);
}

static bool ShowMessageAlert(std::string_view text, bool yes_no, Common::Log::LogType log_type,
                             MsgType style, const char* file, int line)
{
  const char* caption = GetCaption(style);
  if (s_panic_log_handler && (style == MsgType::Warning || style == MsgType::Critical))
  {
    const std::string relative_file = SourceRelativePath(file);
    const std::string diagnostic = SingleLinePanic(std::string(text), relative_file, line);
    s_panic_log_handler(diagnostic.c_str());
    if (auto* logs = Log::LogManager::GetInstance())
    {
      logs->LogWithFullPath(Log::LogLevel::LERROR, log_type, relative_file.c_str(), line,
                            diagnostic.c_str());
    }
  }
  else
  {
    const Log::LogLevel level = GetMessageAlertLogLevel(style);
    // Directly call GenericLogFmt rather than using the normal log macros so that we can use the
    // caller's line file and line number
    Common::Log::GenericLogFmt<2>(level, log_type, file, line, FMT_STRING("{}: {}"), caption, text);
  }

  // Panic alerts.
  if (style == MsgType::Warning && s_abort_on_panic_alert)
  {
    std::abort();
  }

  // Don't ignore questions, especially AskYesNo, PanicYesNo could be ignored
  if (s_msg_handler != nullptr &&
      (s_alert_enabled || style == MsgType::Question || style == MsgType::Critical))
  {
    return s_msg_handler(caption, text.data(), yes_no, style);
  }

  return true;
}

// This is the first stop for gui alerts where the log is updated and the
// correct window is shown, when using fmt
bool MsgAlertFmtImpl(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file,
                     int line, fmt::string_view format, const fmt::format_args& args)
{
  const auto message = fmt::vformat(format, args);

  return ShowMessageAlert(message, yes_no, log_type, style, file, line);
}
}  // namespace Common
