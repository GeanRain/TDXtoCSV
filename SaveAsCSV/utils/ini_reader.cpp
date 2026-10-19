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

#include "ini_reader.h"

IniReader::IniReader(const std::string& path,
                     const std::string& defaultContent)
    : m_path(path),
      m_defaultText(defaultContent)
{
    ensureFileExists();
}

bool IniReader::isValid() const noexcept
{
    DWORD attr = GetFileAttributesA(m_path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

void IniReader::ensureFileExists() noexcept
{
    if (isValid())
        return;

    if (!m_defaultText.empty())
        createDefaultFile();
}

bool IniReader::createDefaultFile() noexcept
{
    HANDLE hFile = CreateFileA(
        m_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    WriteFile(hFile,
              m_defaultText.data(),
              (DWORD)m_defaultText.size(),
              &written,
              nullptr);

    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    return written == m_defaultText.size();
}

std::string IniReader::getString(const char* section,
                                 const char* key,
                                 const char* defaultValue) const
{
    char buf[256]{};

    GetPrivateProfileStringA(
        section,
        key,
        defaultValue,
        buf,
        sizeof(buf),
        m_path.c_str());

    return buf;
}

int IniReader::getInt(const char* section,
                      const char* key,
                      int defaultValue) const noexcept
{
    return GetPrivateProfileIntA(
        section,
        key,
        defaultValue,
        m_path.c_str());
}
