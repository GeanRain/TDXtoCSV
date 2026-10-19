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
#pragma once
#ifndef __LOGGER_H__
#define __LOGGER_H__
#include <windows.h>


#include <string>
#include <windows.h>

enum class LogMode
{
    Disabled = 0,
    DebugView,
    Console,
    File
};

class Logger
{
public:
    static void initialize(const std::string &dllPath, const std::string &prefix = {});
    static void shutdown();
    static void clearConsole();

    static void log(const char *level, const char *fmt, ...);

private:
    static void logDebugView(const char *line);
    static void logConsole(const char *line, DWORD length);
    static void logFile(const char *line, DWORD length);

private:
    static LogMode s_mode;
    static bool s_level_debug_en;
    static bool s_level_info_en;
    static bool s_level_warn_en;
    static bool s_level_error_en;
    static HANDLE s_file;
    static CRITICAL_SECTION s_lock;
};

#endif
