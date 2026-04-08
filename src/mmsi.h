///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
// Copyright (C) 2015-2019 Edouard Griffiths, F4EXB <f4exb06@gmail.com>          //
// Copyright (C) 2021, 2023 Jon Beniston, M7RCE <jon@beniston.com>               //
//                                                                               //
// Ported from SDRangel to standalone C++ (Qt→STL) for ubersdr_dsc               //
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

#ifndef MMSI_H
#define MMSI_H

#include <string>
#include <map>

// Maritime mobile service identities (basically ship identifiers)
// MMSIs defined by ITU-R M.585
// https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.585-9-202205-I!!PDF-E.pdf

class MMSI {

public:

    static std::string getMID(const std::string &mmsi);
    static std::string getCountry(const std::string &mmsi);
    static std::string getCategory(const std::string &mmsi);

private:

    static const std::map<int, std::string> m_mid;
};

#endif /* MMSI_H */
