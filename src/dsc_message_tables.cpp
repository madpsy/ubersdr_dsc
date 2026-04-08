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

// "short" strings are meant to be compatible with YaDDNet

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
    {ROUTINE,       "Routine"},
    {SAFETY,        "Safety"},
    {URGENCY,       "Urgency"},
    {DISTRESS,      "Distress"}
};

std::map<int, std::string> DSCMessage::m_categoryShortStrings = {
    {ROUTINE,       "RTN"},
    {SAFETY,        "SAF"},
    {URGENCY,       "URG"},
    {DISTRESS,      "DIS"}
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
    {NO_REASON,                 "No reason"},
    {CONGESTION,                "Congestion at switching centre"},
    {BUSY,                      "Busy"},
    {QUEUE,                     "Queue indication"},
    {BARRED,                    "Station barred"},
    {NO_OPERATOR,               "No operator available"},
    {OPERATOR_UNAVAILABLE,      "Operator temporarily unavailable"},
    {EQUIPMENT_DISABLED,        "Equipment disabled"},
    {UNABLE_TO_USE_CHANNEL,     "Unable to use proposed channel"},
    {UNABLE_TO_USE_MODE,        "Unable to use proposed mode"},
    {NOT_PARTIES_TO_CONFLICT,   "Ships and aircraft of States not parties to an armed conflict"},
    {MEDICAL_TRANSPORTS,        "Medical transports"},
    {PAY_PHONE,                 "Pay-phone/public call office"},
    {FAX,                       "Facsimile"},
    {NO_INFORMATION_2,          "No information"}
};

std::map<int, std::string> DSCMessage::m_telecommand2ShortStrings = {
    {NO_REASON,                 "NO REASON GIVEN"},
    {CONGESTION,                "CONGESTION AT MARITIME CENTRE"},
    {BUSY,                      "BUSY"},
    {QUEUE,                     "QUEUE INDICATION"},
    {BARRED,                    "STATION BARRED"},
    {NO_OPERATOR,               "NO OPERATOR AVAILABLE"},
    {OPERATOR_UNAVAILABLE,      "OPERATOR TEMPORARILY UNAVAILABLE"},
    {EQUIPMENT_DISABLED,        "EQUIPMENT DISABLED"},
    {UNABLE_TO_USE_CHANNEL,     "UNABLE TO USE PROPOSED CHANNEL"},
    {UNABLE_TO_USE_MODE,        "UNABLE TO USE PROPOSED MODE"},
    {NOT_PARTIES_TO_CONFLICT,   "SHIPS/AIRCRAFT OF STATES NOT PARTIES TO ARMED CONFLICT"},
    {MEDICAL_TRANSPORTS,        "MEDICAL TRANSPORTS"},
    {PAY_PHONE,                 "PAY-PHONE/PUBLIC CALL OFFICE"},
    {FAX,                       "FAX/DATA ACCORDING ITU-R M1081"},
    {NO_INFORMATION_2,          "NOINF"}
};

std::map<int, std::string> DSCMessage::m_distressNatureStrings = {
    {FIRE,              "Fire, explosion"},
    {FLOODING,          "Flooding"},
    {COLLISION,         "Collision"},
    {GROUNDING,         "Grounding"},
    {LISTING,           "Listing"},
    {SINKING,           "Sinking"},
    {ADRIFT,            "Adrift"},
    {UNDESIGNATED,      "Undesignated"},
    {ABANDONING_SHIP,   "Abandoning ship"},
    {PIRACY,            "Piracy, armed attack"},
    {MAN_OVERBOARD,     "Man overboard"},
    {EPIRB,             "EPIRB"}
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
