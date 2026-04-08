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

#ifndef INCLUDE_DSC_MESSAGE_H
#define INCLUDE_DSC_MESSAGE_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <ctime>

// Digital Select Calling - Message parser
// https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.493-15-201901-I!!PDF-E.pdf

class DSCMessage {
public:

    enum FormatSpecifier {
        GEOGRAPHIC_CALL = 102,
        DISTRESS_ALERT = 112,
        GROUP_CALL = 114,
        ALL_SHIPS = 116,
        SELECTIVE_CALL = 120,
        AUTOMATIC_CALL = 123
    };

    enum Category {
        ROUTINE = 100,
        SAFETY = 108,
        URGENCY = 110,
        DISTRESS = 112
    };

    enum FirstTelecommand {
        F3E_G3E_ALL_MODES_TP = 100,
        F3E_G3E_DUPLEX_TP = 101,
        POLLING = 103,
        UNABLE_TO_COMPLY = 104,
        END_OF_CALL = 105,
        DATA = 106,
        J3E_TP = 109,
        DISTRESS_ACKNOWLEDGEMENT = 110,
        DISTRESS_ALERT_RELAY = 112,
        F1B_J2B_TTY_FEC = 113,
        F1B_J2B_TTY_AQR = 115,
        TEST = 118,
        POSITION_UPDATE = 121,
        NO_INFORMATION = 126
    };

    enum SecondTelecommand {
        NO_REASON = 100,
        CONGESTION = 101,
        BUSY = 102,
        QUEUE = 103,
        BARRED = 104,
        NO_OPERATOR = 105,
        OPERATOR_UNAVAILABLE = 106,
        EQUIPMENT_DISABLED = 107,
        UNABLE_TO_USE_CHANNEL = 108,
        UNABLE_TO_USE_MODE = 109,
        NOT_PARTIES_TO_CONFLICT = 110,
        MEDICAL_TRANSPORTS = 111,
        PAY_PHONE = 112,
        FAX = 113,
        NO_INFORMATION_2 = 126
    };

    enum DistressNature {
        FIRE = 100,
        FLOODING = 101,
        COLLISION = 102,
        GROUNDING = 103,
        LISTING = 104,
        SINKING = 105,
        ADRIFT = 106,
        UNDESIGNATED = 107,
        ABANDONING_SHIP = 108,
        PIRACY = 109,
        MAN_OVERBOARD = 110,
        EPIRB = 112
    };

    enum EndOfSignal {
        REQ = 117,
        ACK = 122,
        EOS = 127
    };

    // Static lookup maps
    static std::map<int, std::string> m_formatSpecifierStrings;
    static std::map<int, std::string> m_formatSpecifierShortStrings;
    static std::map<int, std::string> m_categoryStrings;
    static std::map<int, std::string> m_categoryShortStrings;
    static std::map<int, std::string> m_telecommand1Strings;
    static std::map<int, std::string> m_telecommand1ShortStrings;
    static std::map<int, std::string> m_telecommand2Strings;
    static std::map<int, std::string> m_telecommand2ShortStrings;
    static std::map<int, std::string> m_distressNatureStrings;
    static std::map<int, std::string> m_endOfSignalStrings;
    static std::map<int, std::string> m_endOfSignalShortStrings;

    // Check if a value is a valid EndOfSignal
    static bool isEndOfSignal(int value);

    // Message fields
    FormatSpecifier m_formatSpecifier;
    bool m_formatSpecifierMatch;
    std::string m_address;
    bool m_hasAddress;
    int m_addressLatitude;      // For GEOGRAPHIC_CALL
    int m_addressLongitude;
    int m_addressLatAngle;
    int m_addressLonAngle;

    Category m_category;
    bool m_hasCategory;
    std::string m_selfId;
    FirstTelecommand m_telecommand1;
    bool m_hasTelecommand1;
    SecondTelecommand m_telecommand2;
    bool m_hasTelecommand2;

    std::string m_distressId;
    bool m_hasDistressId;

    DistressNature m_distressNature;
    bool m_hasDistressNature;

    std::string m_position;
    bool m_hasPosition;

    int m_frequency1;  // Rx
    bool m_hasFrequency1;
    std::string m_channel1;
    bool m_hasChannel1;
    int m_frequency2;   // Tx
    bool m_hasFrequency2;
    std::string m_channel2;
    bool m_hasChannel2;

    std::string m_number; // Phone number
    bool m_hasNumber;

    std::string m_time;   // "HH:MM" format (ported from QTime)
    bool m_hasTime;

    FirstTelecommand m_subsequenceComms;
    bool m_hasSubsequenceComms;

    EndOfSignal m_eos;
    signed char m_ecc; // Error checking code (parity)
    signed char m_calculatedECC;
    bool m_eccOk;
    bool m_valid; // Data is within defined values

    std::string m_receivedAt;   // ISO 8601 timestamp string of when the message was received
    int64_t m_frequencyHz;      // RF frequency this message was received on (set externally)
    std::vector<unsigned char> m_data;

    // Constructor
    DSCMessage(const std::vector<unsigned char>& data, time_t timestamp);

    // Output methods
    std::string toString(const std::string& separator = " ") const;
    std::string toYaddNetFormat(const std::string& id, int64_t frequency) const;
    std::string toJson() const;

    // Enum-to-string methods
    std::string formatSpecifier(bool shortString = false) const;
    std::string category(bool shortString = false) const;

    static std::string telecommand1(FirstTelecommand telecommand, bool shortString = false);
    static std::string telecommand2(SecondTelecommand telecommand, bool shortString = false);
    static std::string distressNature(DistressNature nature);
    static std::string endOfSignal(EndOfSignal eos, bool shortString = false);

protected:

    std::string symbolsToDigits(const std::vector<unsigned char>& data, int startIdx, int length);
    std::string formatCoordinates(int latitude, int longitude);
    void decode(const std::vector<unsigned char>& data);
    void checkECC(const std::vector<unsigned char>& data);
    void decodeFrequency(const std::vector<unsigned char>& data, int& idx, int& frequency, std::string& channel);
    std::string formatAddress(const std::string& address) const;
    std::string formatCoordinates(const std::string& coords);

    // Helper: format time_t to ISO 8601 string
    static std::string timeToISO8601(time_t t);
};

#endif /* INCLUDE_DSC_MESSAGE_H */
