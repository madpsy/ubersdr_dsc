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
#include "dsc_decoder.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>

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

std::string DSCMessage::timeToISO8601(time_t t)
{
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

DSCMessage::DSCMessage(const std::vector<unsigned char>& data, time_t timestamp) :
    m_formatSpecifier(SELECTIVE_CALL),
    m_formatSpecifierMatch(false),
    m_hasAddress(false),
    m_addressLatitude(0),
    m_addressLongitude(0),
    m_addressLatAngle(0),
    m_addressLonAngle(0),
    m_category(ROUTINE),
    m_hasCategory(false),
    m_telecommand1(NO_INFORMATION),
    m_hasTelecommand1(false),
    m_telecommand2(NO_INFORMATION_2),
    m_hasTelecommand2(false),
    m_hasDistressId(false),
    m_distressNature(UNDESIGNATED),
    m_hasDistressNature(false),
    m_hasPosition(false),
    m_frequency1(0),
    m_hasFrequency1(false),
    m_hasChannel1(false),
    m_frequency2(0),
    m_hasFrequency2(false),
    m_hasChannel2(false),
    m_hasNumber(false),
    m_hasTime(false),
    m_subsequenceComms(NO_INFORMATION),
    m_hasSubsequenceComms(false),
    m_eos(EOS),
    m_ecc(0),
    m_calculatedECC(0),
    m_eccOk(false),
    m_valid(false),
    m_frequencyHz(0),
    m_data(data)
{
    m_receivedAt = timeToISO8601(timestamp);
    decode(data);
}

std::string DSCMessage::symbolsToDigits(const std::vector<unsigned char>& data, int startIdx, int length)
{
    std::string s;
    for (int i = 0; i < length; i++)
    {
        char digits[4];
        snprintf(digits, sizeof(digits), "%02d", (int)data[startIdx+i]);
        s += digits;
    }
    return s;
}

std::string DSCMessage::formatCoordinates(int latitude, int longitude)
{
    std::string lat, lon;
    // UTF-8 degree symbol: \xc2\xb0 = degree
    if (latitude >= 0) {
        lat = strprintf("%d\xc2\xb0N", latitude);
    } else {
        lat = strprintf("%d\xc2\xb0S", -latitude);
    }
    if (longitude >= 0) {
        lon = strprintf("%d\xc2\xb0""E", longitude);
    } else {
        lon = strprintf("%d\xc2\xb0W", -longitude);
    }
    return lat + " " + lon;
}

std::string DSCMessage::formatCoordinates(const std::string& coords)
{
    if (coords == "9999999999")
    {
        return "Not available";
    }
    else
    {
        char quadrant = coords[0];
        // latitude: coords[1..2] degrees, coords[3..4] minutes
        std::string latitude = coords.substr(1, 2) + "\xc2\xb0" + coords.substr(3, 2) + "'";
        // longitude: coords[1..3] degrees (note: original uses mid(1,3)), coords[4..5] minutes
        std::string longitude = coords.substr(1, 3) + "\xc2\xb0" + coords.substr(4, 2) + "'";
        switch (quadrant)
        {
        case '0':
            latitude += 'N';
            longitude += 'E';
            break;
        case '1':
            latitude += 'N';
            longitude += 'W';
            break;
        case '2':
            latitude += 'S';
            longitude += 'E';
            break;
        case '3':
            latitude += 'S';
            longitude += 'W';
            break;
        }
        return latitude + " " + longitude;
    }
}

std::string DSCMessage::formatAddress(const std::string& address) const
{
    // First 9 digits should be MMSI
    // Last digit should always be 0, except for ITU-R M.1080
    if (address.size() >= 10 && address[9] != '0') {
        return address.substr(0, 9) + "-" + address.substr(9, 1);
    } else {
        return address.substr(0, 9);
    }
}

void DSCMessage::checkECC(const std::vector<unsigned char>& data)
{
    m_calculatedECC = 0;
    // Only use one format specifier and one EOS
    for (int i = 1; i < (int)data.size() - 1; i++) {
        m_calculatedECC ^= data[i];
    }
    m_eccOk = m_calculatedECC == m_ecc;
}

void DSCMessage::decodeFrequency(const std::vector<unsigned char>& data, int& idx, int& frequency, std::string& channel)
{
    // No frequency information is indicated by 126 repeated 3 times
    if ((data[idx] == 126) && (data[idx+1] == 126) && (data[idx+2] == 126))
    {
        idx += 3;
        return;
    }

    // Extract frequency digits
    std::string s = symbolsToDigits(data, idx, 3);
    idx += 3;
    if (s[0] == '4')
    {
        s += symbolsToDigits(data, idx, 1);
        idx++;
    }

    if ((s[0] == '0') || (s[0] == '1') || (s[0] == '2'))
    {
        frequency = atoi(s.c_str()) * 100;
    }
    else if (s[0] == '3')
    {
        channel = "CH" + s.substr(1); // HF/MF
    }
    else if (s[0] == '4')
    {
        frequency = atoi(s.substr(1).c_str()) * 10; // Frequency in multiples of 10Hz
    }
    else if (s[0] == '9')
    {
        channel = "CH" + s.substr(2) + "VHF";  // VHF
    }
}

void DSCMessage::decode(const std::vector<unsigned char>& data)
{
    int idx = 0;

    // Format specifier
    m_formatSpecifier = (FormatSpecifier) data[idx++];
    m_formatSpecifierMatch = m_formatSpecifier == (FormatSpecifier) data[idx++];

    // Address and category
    if (m_formatSpecifier != DISTRESS_ALERT)
    {
        if (m_formatSpecifier != ALL_SHIPS)
        {
            m_address = symbolsToDigits(data, idx, 5);
            idx += 5;
            m_hasAddress = true;

            if (m_formatSpecifier == SELECTIVE_CALL)
            {
                m_address = formatAddress(m_address);
            }
            else if (m_formatSpecifier == GEOGRAPHIC_CALL)
            {
                char azimuthSector = m_address[0];
                m_addressLatitude = (m_address[1] - '0') * 10 + (m_address[2] - '0');
                m_addressLongitude = (m_address[3] - '0') * 100
                                   + (m_address[4] - '0') * 10
                                   + (m_address[5] - '0');
                switch (azimuthSector)
                {
                case '0': break;
                case '1': m_addressLongitude = -m_addressLongitude; break;
                case '2': m_addressLatitude = -m_addressLatitude; break;
                case '3':
                    m_addressLongitude = -m_addressLongitude;
                    m_addressLatitude = -m_addressLatitude;
                    break;
                default: break;
                }
                m_addressLatAngle = (m_address[6] - '0') * 10 + (m_address[7] - '0');
                m_addressLonAngle = (m_address[8] - '0') * 10 + (m_address[9] - '0');

                int latitude2 = m_addressLatitude + m_addressLatAngle;
                int longitude2 = m_addressLongitude + m_addressLonAngle;

                m_address = formatCoordinates(m_addressLatitude, m_addressLongitude)
                          + " - "
                          + formatCoordinates(latitude2, longitude2);
            }
        }
        else
        {
            m_hasAddress = false;
        }
        m_category = (Category) data[idx++];
        m_hasCategory = true;
    }
    else
    {
        m_hasAddress = false;
        m_hasCategory = true;
    }

    // Self Id
    m_selfId = symbolsToDigits(data, idx, 5);
    m_selfId = formatAddress(m_selfId);
    idx += 5;

    // Telecommands
    if (m_formatSpecifier != DISTRESS_ALERT)
    {
        m_telecommand1 = (FirstTelecommand) data[idx++];
        m_hasTelecommand1 = true;

        if (m_category != DISTRESS)
        {
            m_telecommand2 = (SecondTelecommand) data[idx++];
            m_hasTelecommand2 = true;
        }
        else
        {
            m_hasTelecommand2 = false;
        }
    }
    else
    {
        m_hasTelecommand1 = false;
        m_hasTelecommand2 = false;
    }

    // ID of source of distress for relays and acks
    if (m_hasCategory && m_category == DISTRESS)
    {
        m_distressId = symbolsToDigits(data, idx, 5);
        m_distressId = formatAddress(m_distressId);
        idx += 5;
        m_hasDistressId = true;
    }
    else
    {
        m_hasDistressId = false;
    }

    if (m_formatSpecifier == DISTRESS_ALERT)
    {
        m_distressNature = (DistressNature) data[idx++];
        m_position = formatCoordinates(symbolsToDigits(data, idx, 5));
        idx += 5;
        m_hasDistressNature = true;
        m_hasPosition = true;

        m_hasFrequency1 = false;
        m_hasChannel1 = false;
        m_hasFrequency2 = false;
        m_hasChannel2 = false;
    }
    else if (m_hasCategory && (m_category != DISTRESS))
    {
        m_hasDistressNature = false;
        if (data[idx] == 55)
        {
            m_position = formatCoordinates(symbolsToDigits(data, idx, 5));
            idx += 5;
            m_hasPosition = true;

            m_hasFrequency1 = false;
            m_hasChannel1 = false;
            m_hasFrequency2 = false;
            m_hasChannel2 = false;
        }
        else
        {
            m_hasPosition = false;
            m_frequency1 = 0;
            decodeFrequency(data, idx, m_frequency1, m_channel1);
            m_hasFrequency1 = m_frequency1 != 0;
            m_hasChannel1 = !m_channel1.empty();

            if (m_formatSpecifier != AUTOMATIC_CALL)
            {
                m_frequency2 = 0;
                decodeFrequency(data, idx, m_frequency2, m_channel2);
                m_hasFrequency2 = m_frequency2 != 0;
                m_hasChannel2 = !m_channel2.empty();
            }
            else
            {
                m_hasFrequency2 = false;
                m_hasChannel2 = false;
            }
        }
    }
    else
    {
        m_hasDistressNature = false;
        m_hasPosition = false;
        m_hasFrequency1 = false;
        m_hasChannel1 = false;
        m_hasFrequency2 = false;
        m_hasChannel2 = false;
    }

    if (m_formatSpecifier == AUTOMATIC_CALL)
    {
        signed char oddEven = data[idx++];
        int len = (int)data.size() - idx - 2; // EOS + ECC
        m_number = symbolsToDigits(data, idx, len);
        idx += len;
        if (oddEven == 105) {
            m_number = m_number.substr(1);
        }
        m_hasNumber = true;
    }
    else
    {
        m_hasNumber = false;
    }

    // Time
    if ((m_formatSpecifier == DISTRESS_ALERT)
        || (m_hasCategory && (m_category == DISTRESS)))
    {
        std::string timeStr = symbolsToDigits(data, idx, 2);
        idx += 2;
        if (timeStr != "8888")
        {
            m_time = timeStr.substr(0, 2) + ":" + timeStr.substr(2, 2);
            m_hasTime = true;
        }
        else
        {
            m_hasTime = false;
        }
    }
    else
    {
        m_hasTime = false;
    }

    // Subsequent communications
    if ((m_formatSpecifier == DISTRESS_ALERT)
        || (m_hasCategory && (m_category == DISTRESS)))
    {
        m_subsequenceComms = (FirstTelecommand)data[idx++];
        m_hasSubsequenceComms = true;
    }
    else
    {
        m_hasSubsequenceComms = false;
    }

    m_eos = (EndOfSignal) data[idx++];
    m_ecc = data[idx++];

    checkECC(data);

    // Indicate message as being invalid if any unexpected data
    bool hasInvalidByte = false;
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == 0xFF) { // -1 as unsigned char
            hasInvalidByte = true;
            break;
        }
    }

    if (   m_formatSpecifierStrings.find((int)m_formatSpecifier) != m_formatSpecifierStrings.end()
        && (!m_hasCategory || m_categoryStrings.find((int)m_category) != m_categoryStrings.end())
        && (!m_hasTelecommand1 || m_telecommand1Strings.find((int)m_telecommand1) != m_telecommand1Strings.end())
        && (!m_hasTelecommand2 || m_telecommand2Strings.find((int)m_telecommand2) != m_telecommand2Strings.end())
        && (!m_hasDistressNature || m_distressNatureStrings.find((int)m_distressNature) != m_distressNatureStrings.end())
        && m_endOfSignalStrings.find((int)m_eos) != m_endOfSignalStrings.end()
        && !hasInvalidByte
        && ((int)data.size() < DSCDecoder::m_maxBytes)
        && m_eccOk
       ) {
        m_valid = true;
    } else {
        m_valid = false;
    }
}
