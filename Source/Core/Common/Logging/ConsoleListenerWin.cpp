// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Logging/ConsoleListener.h"

#include <cstdio>

#include <windows.h>

#include "Common/StringUtil.h"

ConsoleListener::ConsoleListener()
{
  const DWORD type = ::GetFileType(::GetStdHandle(STD_OUTPUT_HANDLE));
  m_stdout_redirected = type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

ConsoleListener::~ConsoleListener()
{
  if (m_stdout_redirected)
    std::fflush(stdout);
}

void ConsoleListener::Log(Common::Log::LogLevel, const char* text)
{
  ::OutputDebugStringW(UTF8ToWString(text).c_str());
  if (m_stdout_redirected)
  {
    std::fputs(text, stdout);
    std::fflush(stdout);
  }
}
