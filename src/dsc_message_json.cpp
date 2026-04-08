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
#include <cmath>

// Helper: snprintf to std::string (local to this TU)
static std::string strprintf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

// Helper: escape a string for JSON output
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char hex[8];
                snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)c);
                out += hex;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string DSCMessage::toString(const std::string& separator) const
{
    std::vector<std::string> s;

    s.push_back("Format specifier: " + formatSpecifier());

    if (m_hasAddress) {
        s.push_back("Address: " + m_address);
    }
    if (m_hasCategory) {
        s.push_back("Category: " + category());
    }

    s.push_back("Self Id: " + m_selfId);

    if (m_hasTelecommand1) {
        s.push_back("Telecommand 1: " + telecommand1(m_telecommand1));
    }
    if (m_hasTelecommand2) {
        s.push_back("Telecommand 2: " + telecommand2(m_telecommand2));
    }

    if (m_hasDistressId) {
        s.push_back("Distress Id: " + m_distressId);
    }
    if (m_hasDistressNature)
    {
        s.push_back("Distress nature: " + distressNature(m_distressNature));
        s.push_back("Distress coordinates: " + m_position);
    }
    else if (m_hasPosition)
    {
        s.push_back("Position: " + m_position);
    }

    if (m_hasFrequency1) {
        s.push_back(strprintf("RX Frequency: %dHz", m_frequency1));
    }
    if (m_hasChannel1) {
        s.push_back("RX Channel: " + m_channel1);
    }
    if (m_hasFrequency2) {
        s.push_back(strprintf("TX Frequency: %dHz", m_frequency2));
    }
    if (m_hasChannel2) {
        s.push_back("TX Channel: " + m_channel2);
    }
    if (m_hasNumber) {
        s.push_back("Phone Number: " + m_number);
    }

    if (m_hasTime) {
        s.push_back("Time: " + m_time);
    }
    if (m_hasSubsequenceComms) {
        s.push_back("Subsequent comms: " + telecommand1(m_subsequenceComms));
    }

    // Join with separator
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (i > 0) result += separator;
        result += s[i];
    }
    return result;
}

std::string DSCMessage::toYaddNetFormat(const std::string& id, int64_t frequency) const
{
    std::vector<std::string> s;

    // rx_id
    s.push_back("[" + id + "]");
    // rx_freq
    float frequencyKHZ = frequency / 1000.0f;
    s.push_back(strprintf("%.1f", frequencyKHZ));
    // fmt
    s.push_back(formatSpecifier(true));
    // to
    if (m_hasAddress)
    {
        if (m_formatSpecifier == GEOGRAPHIC_CALL)
        {
            char ns = m_addressLatitude >= 0 ? 'N' : 'S';
            char ew = m_addressLongitude >= 0 ? 'E' : 'W';
            int lat = abs(m_addressLatitude);
            int lon = abs(m_addressLongitude);
            // UTF-8 degree sign: \xc2\xb0
            s.push_back(strprintf("AREA %02d\xc2\xb0%c=>%02d\xc2\xb0 %03d\xc2\xb0%c=>%02d\xc2\xb0",
                lat, ns, m_addressLatAngle,
                lon, ew, m_addressLonAngle));
        }
        else
        {
            s.push_back(m_address);
        }
    }
    else
    {
        s.push_back("");
    }
    // cat
    s.push_back(category(true));
    // from
    s.push_back(m_selfId);

    // tc1
    if (m_hasTelecommand1) {
        s.push_back(telecommand1(m_telecommand1, true));
    } else {
        s.push_back("--");
    }
    // tc2
    if (m_hasTelecommand2) {
        s.push_back(telecommand2(m_telecommand2, true));
    } else {
        s.push_back("--");
    }
    // freq
    if (m_hasFrequency1 && m_hasFrequency2) {
        s.push_back(strprintf("%07.1f/%07.1fKHz", m_frequency1/1000.0, m_frequency2/1000.0));
    } else if (m_hasFrequency1) {
        s.push_back(strprintf("%07.1fKHz", m_frequency1/1000.0));
    } else if (m_hasFrequency2) {
        s.push_back(strprintf("%07.1fKHz", m_frequency2/1000.0));
    } else if (m_hasChannel1 && m_hasChannel2) {
        s.push_back(m_channel1 + "/" + m_channel2);
    } else if (m_hasChannel1) {
        s.push_back(m_channel1);
    } else if (m_hasChannel2) {
        s.push_back(m_channel2);
    } else {
        s.push_back("--");
    }
    // pos
    if (m_hasPosition) {
        s.push_back(m_position);
    } else {
        s.push_back("--");
    }

    // eos
    s.push_back(endOfSignal(m_eos, true));
    // ecc
    s.push_back(strprintf("ECC %d %s", (int)m_calculatedECC, m_eccOk ? "OK" : "ERR"));

    // Join with semicolons
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (i > 0) result += ";";
        result += s[i];
    }
    return result;
}

std::string DSCMessage::toJson() const
{
    std::string j = "{";

    // Metadata
    j += "\"receivedAt\":\"" + jsonEscape(m_receivedAt) + "\"";
    j += ",\"frequencyHz\":" + strprintf("%lld", (long long)m_frequencyHz);
    j += ",\"valid\":" + std::string(m_valid ? "true" : "false");
    j += ",\"eccOk\":" + std::string(m_eccOk ? "true" : "false");
    j += ",\"ecc\":" + strprintf("%d", (int)m_ecc);
    j += ",\"calculatedECC\":" + strprintf("%d", (int)m_calculatedECC);

    // Format specifier
    j += ",\"formatSpecifier\":\"" + jsonEscape(formatSpecifier()) + "\"";
    j += ",\"formatSpecifierCode\":" + strprintf("%d", (int)m_formatSpecifier);

    // Address
    j += ",\"hasAddress\":" + std::string(m_hasAddress ? "true" : "false");
    if (m_hasAddress) {
        j += ",\"address\":\"" + jsonEscape(m_address) + "\"";
    }

    // Geographic area fields
    if (m_formatSpecifier == GEOGRAPHIC_CALL && m_hasAddress) {
        j += ",\"addressLatitude\":" + strprintf("%d", m_addressLatitude);
        j += ",\"addressLongitude\":" + strprintf("%d", m_addressLongitude);
        j += ",\"addressLatAngle\":" + strprintf("%d", m_addressLatAngle);
        j += ",\"addressLonAngle\":" + strprintf("%d", m_addressLonAngle);
    }

    // Category
    j += ",\"hasCategory\":" + std::string(m_hasCategory ? "true" : "false");
    if (m_hasCategory) {
        j += ",\"category\":\"" + jsonEscape(category()) + "\"";
        j += ",\"categoryCode\":" + strprintf("%d", (int)m_category);
    }

    // Self Id
    j += ",\"selfId\":\"" + jsonEscape(m_selfId) + "\"";

    // Telecommand 1
    j += ",\"hasTelecommand1\":" + std::string(m_hasTelecommand1 ? "true" : "false");
    if (m_hasTelecommand1) {
        j += ",\"telecommand1\":\"" + jsonEscape(telecommand1(m_telecommand1)) + "\"";
        j += ",\"telecommand1Code\":" + strprintf("%d", (int)m_telecommand1);
    }

    // Telecommand 2
    j += ",\"hasTelecommand2\":" + std::string(m_hasTelecommand2 ? "true" : "false");
    if (m_hasTelecommand2) {
        j += ",\"telecommand2\":\"" + jsonEscape(telecommand2(m_telecommand2)) + "\"";
        j += ",\"telecommand2Code\":" + strprintf("%d", (int)m_telecommand2);
    }

    // Distress Id
    j += ",\"hasDistressId\":" + std::string(m_hasDistressId ? "true" : "false");
    if (m_hasDistressId) {
        j += ",\"distressId\":\"" + jsonEscape(m_distressId) + "\"";
    }

    // Distress nature
    j += ",\"hasDistressNature\":" + std::string(m_hasDistressNature ? "true" : "false");
    if (m_hasDistressNature) {
        j += ",\"distressNature\":\"" + jsonEscape(distressNature(m_distressNature)) + "\"";
        j += ",\"distressNatureCode\":" + strprintf("%d", (int)m_distressNature);
    }

    // Position
    j += ",\"hasPosition\":" + std::string(m_hasPosition ? "true" : "false");
    if (m_hasPosition) {
        j += ",\"position\":\"" + jsonEscape(m_position) + "\"";
    }

    // Frequency 1 (Rx)
    j += ",\"hasFrequency1\":" + std::string(m_hasFrequency1 ? "true" : "false");
    if (m_hasFrequency1) {
        j += ",\"frequency1\":" + strprintf("%d", m_frequency1);
    }
    j += ",\"hasChannel1\":" + std::string(m_hasChannel1 ? "true" : "false");
    if (m_hasChannel1) {
        j += ",\"channel1\":\"" + jsonEscape(m_channel1) + "\"";
    }

    // Frequency 2 (Tx)
    j += ",\"hasFrequency2\":" + std::string(m_hasFrequency2 ? "true" : "false");
    if (m_hasFrequency2) {
        j += ",\"frequency2\":" + strprintf("%d", m_frequency2);
    }
    j += ",\"hasChannel2\":" + std::string(m_hasChannel2 ? "true" : "false");
    if (m_hasChannel2) {
        j += ",\"channel2\":\"" + jsonEscape(m_channel2) + "\"";
    }

    // Phone number
    j += ",\"hasNumber\":" + std::string(m_hasNumber ? "true" : "false");
    if (m_hasNumber) {
        j += ",\"number\":\"" + jsonEscape(m_number) + "\"";
    }

    // Time
    j += ",\"hasTime\":" + std::string(m_hasTime ? "true" : "false");
    if (m_hasTime) {
        j += ",\"time\":\"" + jsonEscape(m_time) + "\"";
    }

    // Subsequent comms
    j += ",\"hasSubsequenceComms\":" + std::string(m_hasSubsequenceComms ? "true" : "false");
    if (m_hasSubsequenceComms) {
        j += ",\"subsequenceComms\":\"" + jsonEscape(telecommand1(m_subsequenceComms)) + "\"";
        j += ",\"subsequenceCommsCode\":" + strprintf("%d", (int)m_subsequenceComms);
    }

    // End of signal
    j += ",\"eos\":\"" + jsonEscape(endOfSignal(m_eos)) + "\"";
    j += ",\"eosCode\":" + strprintf("%d", (int)m_eos);

    // Raw data as hex
    j += ",\"rawData\":\"";
    for (size_t i = 0; i < m_data.size(); i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02x", m_data[i]);
        j += hex;
    }
    j += "\"";

    j += "}";
    return j;
}
