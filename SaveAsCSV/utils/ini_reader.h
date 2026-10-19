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
#ifndef __INIREADER_H__
#define __INIREADER_H__
#include <windows.h>


#include <string>
#include <windows.h>

class IniReader
{
public:
    IniReader(const std::string &path,
              const std::string &defaultContent = "");

    bool isValid() const noexcept;

    std::string getString(const char *section,
                          const char *key,
                          const char *defaultValue) const;

    int getInt(const char *section,
               const char *key,
               int defaultValue) const noexcept;

private:
    void ensureFileExists() noexcept;
    bool createDefaultFile() noexcept;

private:
    std::string m_path;
    std::string m_defaultText;
};

#endif
