/*****************************************************************************
 * Visual K-Line Analysing System For SaveAsCSV
 * Copyright (C) 2025, GeanRain

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include "logger.h"
#include "ini_reader.h"

#include "../common.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

LogMode Logger::s_mode = LogMode::Disabled;
bool Logger::s_level_debug_en = false;
bool Logger::s_level_info_en = false;
bool Logger::s_level_warn_en = false;
bool Logger::s_level_error_en = false;
HANDLE Logger::s_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION Logger::s_lock;

static std::string dirname(const std::string &path)
{
    size_t p = path.find_last_of("\\/");
    return (p == std::string::npos) ? path : path.substr(0, p);
}

void Logger::initialize(const std::string &dllPath, const std::string &prefix)
{
    InitializeCriticalSection(&s_lock);

    std::string iniPath = dllPath + "\\" + INIREADER_FILENAME;

    IniReader ini(iniPath, kDefaultIni);

    std::string mode = ini.getString("log", "mode", "debugview");

    if (mode == "console")
        s_mode = LogMode::Console;
    else if (mode == "file")
        s_mode = LogMode::File;
    else if (mode == "debugview")
        s_mode = LogMode::DebugView;
    else
        s_mode = LogMode::Disabled;

    if (s_mode == LogMode::File)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);

        char day_time[64];
        sprintf_s(day_time, "%02d%02d%02d_%02d%02d_",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute);

        std::string fileName =
            ini.getString("log", "file", "saveAsCVS.log");

        std::string fullPath = dllPath + "\\" + day_time + prefix + fileName;

        s_file = CreateFileA(
            fullPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }

    if (s_mode == LogMode::Console)
        AllocConsole();

    int level = ini.getInt("log", "level", 2);

    switch (level)
    {
    case 0:
    {
        s_level_debug_en = true;
        s_level_info_en = true;
        s_level_warn_en = true;
        s_level_error_en = true;
    }
    break;
    case 1:
    {
        s_level_debug_en = false;
        s_level_info_en = true;
        s_level_warn_en = true;
        s_level_error_en = true;
    }
    break;
    case 2:
    {
        s_level_debug_en = false;
        s_level_info_en = false;
        s_level_warn_en = true;
        s_level_error_en = true;
    }
    break;
    case 3:
    {
        s_level_debug_en = false;
        s_level_info_en = false;
        s_level_warn_en = false;
        s_level_error_en = true;
    }
    break;
    }
}

void Logger::shutdown()
{
    EnterCriticalSection(&s_lock);

    if (s_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
    }

    if (s_mode == LogMode::Console)
        FreeConsole();

    LeaveCriticalSection(&s_lock);
    DeleteCriticalSection(&s_lock);
}

void Logger::clearConsole()
{
    // 只在 Console 模式下生效
    if (s_mode != LogMode::Console)
        return;

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE || hConsole == nullptr)
        return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi))
        return;

    DWORD cellCount =
        csbi.dwSize.X * csbi.dwSize.Y;

    COORD home = {0, 0};
    DWORD written = 0;

    EnterCriticalSection(&s_lock);

    // 清空字符
    FillConsoleOutputCharacterA(
        hConsole,
        ' ',
        cellCount,
        home,
        &written);

    // 清空属性
    FillConsoleOutputAttribute(
        hConsole,
        csbi.wAttributes,
        cellCount,
        home,
        &written);

    // 光标复位
    SetConsoleCursorPosition(hConsole, home);

    LeaveCriticalSection(&s_lock);
}

void Logger::log(const char *level, const char *fmt, ...)
{
    if (s_mode == LogMode::Disabled)
        return;

    do
    {
        if (std::strcmp(level, "DEBUG") == 0)
        {
            if (!s_level_debug_en)
                return;
            else
                break;
        }
        if (std::strcmp(level, "INFO") == 0)
        {
            if (!s_level_info_en)
                return;
            else
                break;
        }
        if (std::strcmp(level, "WARN") == 0)
        {
            if (!s_level_warn_en)
                return;
            else
                break;
        }
        if (std::strcmp(level, "ERROR") == 0)
        {
            if (!s_level_error_en)
                return;
            else
                break;
        }
    } while (0);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char header[64];
    sprintf_s(header, "[%02d:%02d:%02d][%s] ",
              st.wHour, st.wMinute, st.wSecond, level);

    // 格式化可变参数消息
    char message[1024]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // 内存不足时日志仍须可写，整条输出不申请 C++ 堆内存。
    char line[sizeof(header) + sizeof(message) + 1]{};
    snprintf(line, sizeof(line), "%s%s\n", header, message);
    const auto length = static_cast<DWORD>(std::strlen(line));

    EnterCriticalSection(&s_lock);

    switch (s_mode)
    {
    case LogMode::DebugView:
        logDebugView(line);
        break;
    case LogMode::Console:
        logConsole(line, length);
        break;
    case LogMode::File:
        logFile(line, length);
        break;
    default:
        break;
    }

    LeaveCriticalSection(&s_lock);
}

void Logger::logDebugView(const char *line)
{
    OutputDebugStringA(line);
}

void Logger::logConsole(const char *line, DWORD length)
{
    DWORD written;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE),
                  line,
                  length,
                  &written,
                  nullptr);
}

void Logger::logFile(const char *line, DWORD length)
{
    if (s_file == INVALID_HANDLE_VALUE)
        return;

    DWORD written;
    WriteFile(s_file,
              line,
              length,
              &written,
              nullptr);
}
