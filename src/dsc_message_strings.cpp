///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2023 Jon Beniston, M7RCE <jon@beniston.com>                     //
// Ported to standalone C++ (Qt->STL) for ubersdr_dsc                            //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include "dsc_message.h"
#include <cstdio>
#include <cstdarg>

// Helper: snprintf to std::string (local to this TU)
static std::string strprintf(const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

std::string DSCMessage::formatSpecifier(bool shortString) const
{
    if (shortString)
    {
        auto it = m_formatSpecifierShortStrings.find((int)m_formatSpecifier);
        if (it != m_formatSpecifierShortStrings.end()) {
            return it->second;
        } else {
            return "UNK/ERR";
        }
    }
    else
    {
        auto it = m_formatSpecifierStrings.find((int)m_formatSpecifier);
        if (it != m_formatSpecifierStrings.end()) {
            return it->second;
        } else {
            return strprintf("Unknown (%d)", (int)m_formatSpecifier);
        }
    }
}

std::string DSCMessage::category(bool shortString) const
{
    if (shortString)
    {
        auto it = m_categoryShortStrings.find((int)m_category);
        if (it != m_categoryShortStrings.end()) {
            return it->second;
        } else {
            return "UNK/ERR";
        }
    }
    else
    {
        if (!m_hasCategory) {
            return "N/A";
        }
        auto it = m_categoryStrings.find((int)m_category);
        if (it != m_categoryStrings.end()) {
            return it->second;
        } else {
            return strprintf("Unknown (%d)", (int)m_category);
        }
    }
}

std::string DSCMessage::telecommand1(FirstTelecommand telecommand, bool shortString)
{
    if (shortString)
    {
        auto it = m_telecommand1ShortStrings.find((int)telecommand);
        if (it != m_telecommand1ShortStrings.end()) {
            return it->second;
        } else {
            return "UNK/ERR";
        }
    }
    else
    {
        auto it = m_telecommand1Strings.find((int)telecommand);
        if (it != m_telecommand1Strings.end()) {
            return it->second;
        } else {
            return strprintf("Unknown (%d)", (int)telecommand);
        }
    }
}

std::string DSCMessage::telecommand2(SecondTelecommand telecommand, bool shortString)
{
    if (shortString)
    {
        auto it = m_telecommand2ShortStrings.find((int)telecommand);
        if (it != m_telecommand2ShortStrings.end()) {
            return it->second;
        } else {
            return "UNK/ERR";
        }
    }
    else
    {
        auto it = m_telecommand2Strings.find((int)telecommand);
        if (it != m_telecommand2Strings.end()) {
            return it->second;
        } else {
            return strprintf("Unknown (%d)", (int)telecommand);
        }
    }
}

std::string DSCMessage::distressNature(DistressNature nature)
{
    auto it = m_distressNatureStrings.find((int)nature);
    if (it != m_distressNatureStrings.end()) {
        return it->second;
    } else {
        return strprintf("Unknown (%d)", (int)nature);
    }
}

std::string DSCMessage::endOfSignal(EndOfSignal eos, bool shortString)
{
    if (shortString)
    {
        auto it = m_endOfSignalShortStrings.find((int)eos);
        if (it != m_endOfSignalShortStrings.end()) {
            return it->second;
        } else {
            return "UNK/ERR";
        }
    }
    else
    {
        auto it = m_endOfSignalStrings.find((int)eos);
        if (it != m_endOfSignalStrings.end()) {
            return it->second;
        } else {
            return strprintf("Unknown (%d)", (int)eos);
        }
    }
}
