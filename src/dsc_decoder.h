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

#ifndef INCLUDE_DSC_DECODER_H
#define INCLUDE_DSC_DECODER_H

#include <vector>
#include <cstdint>

// Digital Select Calling - Symbol-level decoder
// https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.493-15-201901-I!!PDF-E.pdf

class DSCDecoder {

public:

    void init(int offset);
    bool decodeBits(int bits);
    std::vector<unsigned char> getMessage() const { return m_bytes; }
    int getErrors() const { return m_errors; }

    static int m_maxBytes;

    // Phasing pattern table (ported from DSCDemodSink::m_phasingPatterns)
    struct PhasingPattern {
        unsigned int m_pattern;
        int m_offset;
    };
    static const PhasingPattern m_phasingPatterns[];
    static const int m_phasingPatternsSize;

private:

    static const int BUFFER_SIZE = 3;
    signed char m_buf[3];
    enum State {
        PHASING,
        FILL_DX,
        FILL_RX,
        DX,
        RX,
        DX_EOS,
        RX_EOS,
        DONE,
        NO_EOS
    } m_state;
    int m_idx;
    int m_errors;
    int m_phaseIdx;
    bool m_eos;
    static const signed char m_expectedSymbols[];

    std::vector<unsigned char> m_bytes;

    bool decodeSymbol(signed char symbol);
    static signed char bitsToSymbol(unsigned int bits);
    static unsigned char reverse(unsigned char b);
    signed char selectSymbol(signed char dx, signed char rx);

};

#endif /* INCLUDE_DSC_DECODER_H */
