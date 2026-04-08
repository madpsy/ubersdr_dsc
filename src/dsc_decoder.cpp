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

#include "dsc_decoder.h"
#include "dsc_message.h"
#include "popcount.h"

// Phasing pattern table (ported from DSCDemodSink::m_phasingPatterns in dscdemodsink.cpp)
// Each entry is a 30-bit pattern (3 consecutive 10-bit symbols) and the phasing offset
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

const int DSCDecoder::m_phasingPatternsSize = sizeof(DSCDecoder::m_phasingPatterns) / sizeof(DSCDecoder::m_phasingPatterns[0]);

// Doesn't include 125 111 125 as these will have been detected already, in DSDDemodSink
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
        // Save, EOS symbol
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

// Convert 10 bits to a symbol
// Returns -1 if error detected
signed char DSCDecoder::bitsToSymbol(unsigned int bits)
{
    signed char data = reverse(bits >> 3) >> 1;
    int zeros = 7-popcount(data);
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
