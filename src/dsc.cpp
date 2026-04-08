///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2023 Jon Beniston, M7RCE <jon@beniston.com>                     //
// Ported to standalone C++ (Qt→STL) for ubersdr_dsc                             //
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

// Combined implementation of DSCDecoder + DSCMessage (Qt-stripped).
// This file replaces the original Qt-dependent src/dsc.cpp from SDRangel and
// the separate dsc_decoder.cpp / dsc_message*.cpp translation units.

#include "dsc.h"
#include "popcount.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>

// ---------------------------------------------------------------------------
// Local helpers (file-scope)
// ---------------------------------------------------------------------------

static std::string strprintf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

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

// ===========================================================================
// DSCDecoder implementation
// ===========================================================================

// Phasing pattern table (ported from DSCDemodSink::m_phasingPatterns)
const DSCDecoder::PhasingPattern DSCDecoder::m_phasingPatterns[] = {
    {0b1011111001'1111011001'1011111001u, 9},      // 125 111 125
    {0b1111011001'1011111001'0111011010u, 8},      // 111 125 110
    {0b1011111001'0111011010'1011111001u, 7},      // 125 110 125
    {0b0111011010'1011111001'1011011010u, 6},      // 110 125 109
    {0b1011111001'1011011010'1011111001u, 5},      // 125 109 125
    {0b1011011010'1011111001'0011011011u, 4},      // 109 125 108
    {0b1011111001'0011011011'1011111001u, 3},      // 125 108 125
    {0b0011011011'1011111001'1101011010u, 2},      // 108 125 107
    {0b1011111001'1101011010'1011111001u, 1},      // 125 107 125
    {0b1101011010'1011111001'0101011011u, 0},      // 107 125 106
};

const int DSCDecoder::m_phasingPatternsSize =
    sizeof(DSCDecoder::m_phasingPatterns) / sizeof(DSCDecoder::m_phasingPatterns[0]);

// Doesn't include 125 111 125 as these will have been detected already, in DSCDemodSink
const signed char DSCDecoder::m_expectedSymbols[] = {
    110,
    125, 109,
    125, 108,
    125, 107,
    125, 106
};

int DSCDecoder::m_maxBytes = 40; // Max bytes in any message

void DSCDecoder::init(int offset)
{
    if (offset == 0)
    {
        m_state = FILL_DX;
    }
    else
    {
        m_phaseIdx = offset;
        m_state = PHASING;
    }
    m_idx = 0;
    m_errors = 0;
    m_bytes.clear();
    m_eos = false;
}

bool DSCDecoder::decodeSymbol(signed char symbol)
{
    bool ret = false;

    switch (m_state)
    {
    case PHASING:
        // Check if received phasing signals are as expected
        if (symbol != m_expectedSymbols[9-m_phaseIdx]) {
            m_errors++;
        }
        m_phaseIdx--;
        if (m_phaseIdx == 0) {
            m_state = FILL_DX;
        }
        break;

    case FILL_DX:
        // Fill up buffer
        m_buf[m_idx++] = symbol;
        if (m_idx == BUFFER_SIZE)
        {
            m_state = RX;
            m_idx = 0;
        }
        else
        {
            m_state = FILL_RX;
        }
        break;

    case FILL_RX:
        if (   ((m_idx == 1) && (symbol != 106))
            || ((m_idx == 2) && (symbol != 105))
           )
        {
            m_errors++;
        }
        m_state = FILL_DX;
        break;

    case RX:
        {
            signed char a = selectSymbol(m_buf[m_idx], symbol);

            if (DSCMessage::isEndOfSignal((int)a)) {
                m_state = DX_EOS;
            } else {
                m_state = DX;
            }

            if ((int)m_bytes.size() > m_maxBytes)
            {
                ret = true;
                m_state = NO_EOS;
            }
        }
        break;

    case DX:
        // Save received character in buffer
        m_buf[m_idx] = symbol;
        m_idx = (m_idx + 1) % BUFFER_SIZE;
        m_state = RX;
        break;

    case DX_EOS:
        // Save EOS symbol
        m_buf[m_idx] = symbol;
        m_idx = (m_idx + 1) % BUFFER_SIZE;
        m_state = RX_EOS;
        break;

    case RX_EOS:
        selectSymbol(m_buf[m_idx], symbol);
        m_state = DONE;
        ret = true;
        break;

    case DONE:
    case NO_EOS:
        break;
    }

    return ret;
}

// Reverse order of bits in a byte
unsigned char DSCDecoder::reverse(unsigned char b)
{
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}

// Convert 10 bits to a symbol; returns -1 if error detected
signed char DSCDecoder::bitsToSymbol(unsigned int bits)
{
    signed char data = reverse(bits >> 3) >> 1;
    int zeros = 7 - popcount(data);
    int expectedZeros = bits & 0x7;
    if (zeros == expectedZeros) {
        return data;
    } else {
        return -1;
    }
}

// Decode 10-bits to symbols then remove errors using repeated symbols
bool DSCDecoder::decodeBits(int bits)
{
    signed char symbol = bitsToSymbol(bits);
    return decodeSymbol(symbol);
}

// Select time diversity symbol without errors
signed char DSCDecoder::selectSymbol(signed char dx, signed char rx)
{
    signed char s;
    if (dx != -1)
    {
        s = dx;  // First received character has no detectable error
        if (dx != rx) {
            m_errors++;
        }
    }
    else if (rx != -1)
    {
        s = rx;  // Second received character has no detectable error
        m_errors++;
    }
    else
    {
        s = '*'; // Both received characters have errors
        m_errors += 2;
    }
    m_bytes.push_back((unsigned char)s);

    return s;
}

// ===========================================================================
// DSCMessage — static lookup tables  ("short" strings compatible with YaDDNet)
// ===========================================================================

std::map<int, std::string> DSCMessage::m_formatSpecifierStrings = {
    {GEOGRAPHIC_CALL, "Geographic call"},
    {DISTRESS_ALERT,  "Distress alert"},
    {GROUP_CALL,      "Group call"},
    {ALL_SHIPS,       "All ships"},
    {SELECTIVE_CALL,  "Selective call"},
    {AUTOMATIC_CALL,  "Automatic call"}
};

std::map<int, std::string> DSCMessage::m_formatSpecifierShortStrings = {
    {GEOGRAPHIC_CALL, "AREA"},
    {DISTRESS_ALERT,  "DIS"},
    {GROUP_CALL,      "GRP"},
    {ALL_SHIPS,       "ALL"},
    {SELECTIVE_CALL,  "SEL"},
    {AUTOMATIC_CALL,  "AUT"}
};

std::map<int, std::string> DSCMessage::m_categoryStrings = {
    {ROUTINE,  "Routine"},
    {SAFETY,   "Safety"},
    {URGENCY,  "Urgency"},
    {DISTRESS, "Distress"}
};

std::map<int, std::string> DSCMessage::m_categoryShortStrings = {
    {ROUTINE,  "RTN"},
    {SAFETY,   "SAF"},
    {URGENCY,  "URG"},
    {DISTRESS, "DIS"}
};

std::map<int, std::string> DSCMessage::m_telecommand1Strings = {
    {F3E_G3E_ALL_MODES_TP,     "F3E (FM speech)/G3E (phase modulated speech) all modes telephony"},
    {F3E_G3E_DUPLEX_TP,        "F3E (FM speech)/G3E (phase modulated speech) duplex telephony"},
    {POLLING,                  "Polling"},
    {UNABLE_TO_COMPLY,         "Unable to comply"},
    {END_OF_CALL,              "End of call"},
    {DATA,                     "Data"},
    {J3E_TP,                   "J3E (SSB) telephony"},
    {DISTRESS_ACKNOWLEDGEMENT, "Distress acknowledgement"},
    {DISTRESS_ALERT_RELAY,     "Distress alert relay"},
    {F1B_J2B_TTY_FEC,          "F1B (FSK) J2B (FSK via SSB) TTY FEC"},
    {F1B_J2B_TTY_AQR,          "F1B (FSK) J2B (FSK via SSB) TTY AQR"},
    {TEST,                     "Test"},
    {POSITION_UPDATE,          "Position update"},
    {NO_INFORMATION,           "No information"}
};

std::map<int, std::string> DSCMessage::m_telecommand1ShortStrings = {
    {F3E_G3E_ALL_MODES_TP,     "F3E/G3E"},
    {F3E_G3E_DUPLEX_TP,        "F3E/G3E, Duplex TP"},
    {POLLING,                  "POLL"},
    {UNABLE_TO_COMPLY,         "UNABLE TO COMPLY"},
    {END_OF_CALL,              "EOC"},
    {DATA,                     "DATA"},
    {J3E_TP,                   "J3E TP"},
    {DISTRESS_ACKNOWLEDGEMENT, "DISTRESS ACK"},
    {DISTRESS_ALERT_RELAY,     "DISTRESS RELAY"},
    {F1B_J2B_TTY_FEC,          "F1B/J2B TTY-FEC"},
    {F1B_J2B_TTY_AQR,          "F1B/J2B TTY-ARQ"},
    {TEST,                     "TEST"},
    {POSITION_UPDATE,          "POSUPD"},
    {NO_INFORMATION,           "NOINF"}
};

std::map<int, std::string> DSCMessage::m_telecommand2Strings = {
    {NO_REASON,               "No reason"},
    {CONGESTION,              "Congestion at switching centre"},
    {BUSY,                    "Busy"},
    {QUEUE,                   "Queue indication"},
    {BARRED,                  "Station barred"},
    {NO_OPERATOR,             "No operator available"},
    {OPERATOR_UNAVAILABLE,    "Operator temporarily unavailable"},
    {EQUIPMENT_DISABLED,      "Equipment disabled"},
    {UNABLE_TO_USE_CHANNEL,   "Unable to use proposed channel"},
    {UNABLE_TO_USE_MODE,      "Unable to use proposed mode"},
    {NOT_PARTIES_TO_CONFLICT, "Ships and aircraft of States not parties to an armed conflict"},
    {MEDICAL_TRANSPORTS,      "Medical transports"},
    {PAY_PHONE,               "Pay-phone/public call office"},
    {FAX,                     "Facsimile"},
    {NO_INFORMATION_2,        "No information"}
};

std::map<int, std::string> DSCMessage::m_telecommand2ShortStrings = {
    {NO_REASON,               "NO REASON GIVEN"},
    {CONGESTION,              "CONGESTION AT MARITIME CENTRE"},
    {BUSY,                    "BUSY"},
    {QUEUE,                   "QUEUE INDICATION"},
    {BARRED,                  "STATION BARRED"},
    {NO_OPERATOR,             "NO OPERATOR AVAILABLE"},
    {OPERATOR_UNAVAILABLE,    "OPERATOR TEMPORARILY UNAVAILABLE"},
    {EQUIPMENT_DISABLED,      "EQUIPMENT DISABLED"},
    {UNABLE_TO_USE_CHANNEL,   "UNABLE TO USE PROPOSED CHANNEL"},
    {UNABLE_TO_USE_MODE,      "UNABLE TO USE PROPOSED MODE"},
    {NOT_PARTIES_TO_CONFLICT, "SHIPS/AIRCRAFT OF STATES NOT PARTIES TO ARMED CONFLICT"},
    {MEDICAL_TRANSPORTS,      "MEDICAL TRANSPORTS"},
    {PAY_PHONE,               "PAY-PHONE/PUBLIC CALL OFFICE"},
    {FAX,                     "FAX/DATA ACCORDING ITU-R M1081"},
    {NO_INFORMATION_2,        "NOINF"}
};

std::map<int, std::string> DSCMessage::m_distressNatureStrings = {
    {FIRE,           "Fire, explosion"},
    {FLOODING,       "Flooding"},
    {COLLISION,      "Collision"},
    {GROUNDING,      "Grounding"},
    {LISTING,        "Listing"},
    {SINKING,        "Sinking"},
    {ADRIFT,         "Adrift"},
    {UNDESIGNATED,   "Undesignated"},
    {ABANDONING_SHIP,"Abandoning ship"},
    {PIRACY,         "Piracy, armed attack"},
    {MAN_OVERBOARD,  "Man overboard"},
    {EPIRB,          "EPIRB"}
};

std::map<int, std::string> DSCMessage::m_endOfSignalStrings = {
    {REQ, "Req ACK"},
    {ACK, "ACK"},
    {EOS, "EOS"}
};

std::map<int, std::string> DSCMessage::m_endOfSignalShortStrings = {
    {REQ, "REQ"},
    {ACK, "ACK"},
    {EOS, "EOS"}
};

bool DSCMessage::isEndOfSignal(int value)
{
    return m_endOfSignalStrings.find(value) != m_endOfSignalStrings.end();
}

// ===========================================================================
// DSCMessage — constructor and helpers
// ===========================================================================

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
    // UTF-8 degree symbol: \xc2\xb0
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
        std::string latitude  = coords.substr(1, 2) + "\xc2\xb0" + coords.substr(3, 2) + "'";
        // longitude: coords[1..3] degrees, coords[4..5] minutes
        std::string longitude = coords.substr(1, 3) + "\xc2\xb0" + coords.substr(4, 2) + "'";
        switch (quadrant)
        {
        case '0': latitude += 'N'; longitude += 'E'; break;
        case '1': latitude += 'N'; longitude += 'W'; break;
        case '2': latitude += 'S'; longitude += 'E'; break;
        case '3': latitude += 'S'; longitude += 'W'; break;
        default: break;
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
                m_addressLatitude  = (m_address[1] - '0') * 10 + (m_address[2] - '0');
                m_addressLongitude = (m_address[3] - '0') * 100
                                   + (m_address[4] - '0') * 10
                                   + (m_address[5] - '0');
                switch (azimuthSector)
                {
                case '0': break;
                case '1': m_addressLongitude = -m_addressLongitude; break;
                case '2': m_addressLatitude  = -m_addressLatitude;  break;
                case '3':
                    m_addressLongitude = -m_addressLongitude;
                    m_addressLatitude  = -m_addressLatitude;
                    break;
                default: break;
                }
                m_addressLatAngle = (m_address[6] - '0') * 10 + (m_address[7] - '0');
                m_addressLonAngle = (m_address[8] - '0') * 10 + (m_address[9] - '0');

                int latitude2  = m_addressLatitude  + m_addressLatAngle;
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
        m_hasChannel1   = false;
        m_hasFrequency2 = false;
        m_hasChannel2   = false;
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
            m_hasChannel1   = false;
            m_hasFrequency2 = false;
            m_hasChannel2   = false;
        }
        else
        {
            m_hasPosition = false;
            m_frequency1 = 0;
            decodeFrequency(data, idx, m_frequency1, m_channel1);
            m_hasFrequency1 = m_frequency1 != 0;
            m_hasChannel1   = !m_channel1.empty();

            if (m_formatSpecifier != AUTOMATIC_CALL)
            {
                m_frequency2 = 0;
                decodeFrequency(data, idx, m_frequency2, m_channel2);
                m_hasFrequency2 = m_frequency2 != 0;
                m_hasChannel2   = !m_channel2.empty();
            }
            else
            {
                m_hasFrequency2 = false;
                m_hasChannel2   = false;
            }
        }
    }
    else
    {
        m_hasDistressNature = false;
        m_hasPosition       = false;
        m_hasFrequency1     = false;
        m_hasChannel1       = false;
        m_hasFrequency2     = false;
        m_hasChannel2       = false;
    }

    if (m_formatSpecifier == AUTOMATIC_CALL)
    {
        signed char oddEven = data[idx++];
        int len = (int)data.size() - idx - 2; // EOS + ECC
        m_number = symbolsToDigits(data, idx, len);
        idx += len;
        if (oddEven == 105) {
            m_number = m_number.substr(1); // Drop leading digit (odd number)
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

///////////////////////////////////////////////////////////////////////////////////
// DSCMessage string methods
///////////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////////
// DSCMessage JSON / toString methods
///////////////////////////////////////////////////////////////////////////////////

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
