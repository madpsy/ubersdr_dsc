/* -*- c++ -*- */
/*
 * dsc_rx.cpp — DSC (Digital Selective Calling) FSK receiver
 *
 * Demodulator ported from SDRangel DSCDemodSink (Jon Beniston, M7RCE):
 *   Copyright (C) 2023 Jon Beniston, M7RCE
 *
 * SDRangel operates at DSCDEMOD_CHANNEL_SAMPLE_RATE = 1000 Hz internally.
 * ubersdr delivers IQ at 10000 Hz, so all sample-count constants are
 * multiplied by 10 (RATE_SCALE = 10).
 *
 * The algorithm is otherwise a direct translation of dscdemodsink.cpp:
 *   - Complex exponential table at FREQUENCY_SHIFT/2 = 85 Hz
 *   - Two Lowpass<Complex> FIR filters (301 taps, cutoff = BAUD_RATE * 1.1)
 *   - Two MovingMaximum envelope trackers (window = samplesPerBit * 8)
 *   - ATC: bias = abs - 0.5 * env
 *   - biasedData = bias_mark - bias_space
 *   - Clock recovery: on RISING EDGE (0→1) only, pull 25% toward zero
 *   - Sample bit when clockCount >= samplesPerBit/2 - 1
 *   - Phasing detection: exact 30-bit pattern match
 *   - Symbol decoding: every 10 bits via DSCDecoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsc_rx.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <numeric>

// -----------------------------------------------------------------------
// DSC / SDRangel constants
// -----------------------------------------------------------------------
static const int    DSCDEMOD_CHANNEL_SAMPLE_RATE = 1000;   // SDRangel internal rate
static const int    DSCDEMOD_BAUD_RATE           = 100;
static const int    DSCDEMOD_FREQUENCY_SHIFT     = 170;    // Hz total FSK shift
static const int    DSCDEMOD_EXP_LENGTH          = 600;    // SDRangel exp table length at 1000 Hz

// Scale factor: ubersdr delivers 10000 Hz, SDRangel uses 1000 Hz
static const int    RATE_SCALE = 10;   // 10000 / 1000

// Derived constants at 10000 Hz
static const int    EXP_LENGTH    = DSCDEMOD_EXP_LENGTH * RATE_SCALE;  // 6000
static const int    SAMPLES_PER_BIT = (DSCDEMOD_CHANNEL_SAMPLE_RATE * RATE_SCALE)
                                      / DSCDEMOD_BAUD_RATE;            // 100

// -----------------------------------------------------------------------
// SimpleLowpass — windowed-sinc FIR lowpass for complex samples
// Uses a circular buffer (like SDRangel's Lowpass<Complex>) for efficiency.
// -----------------------------------------------------------------------
void SimpleLowpass::create(int taps, double fc)
{
    // taps must be odd
    if (taps % 2 == 0) taps++;
    int half = taps / 2;

    m_coeffs.resize(taps);
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        int n = i - half;
        double sinc = (n == 0) ? 1.0
                                : std::sin(M_PI * n * 2.0 * fc) / (M_PI * n * 2.0 * fc);
        // Blackman window (same as SDRangel's Lowpass)
        double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * i / (taps - 1))
                        + 0.08 * std::cos(4.0 * M_PI * i / (taps - 1));
        m_coeffs[i] = sinc * w;
        sum += m_coeffs[i];
    }
    for (auto &c : m_coeffs) c /= sum;

    // Circular buffer
    m_buf.assign(taps, cmplx(0.0, 0.0));
    m_pos = 0;
}

cmplx SimpleLowpass::filter(cmplx in)
{
    // Write new sample into circular buffer
    m_buf[m_pos] = in;
    m_pos = (m_pos + 1) % (int)m_coeffs.size();

    // Convolve: oldest sample is at m_pos, newest is at m_pos-1 (mod size)
    cmplx out(0.0, 0.0);
    int sz = (int)m_coeffs.size();
    for (int i = 0; i < sz; i++) {
        int idx = (m_pos + i) % sz;
        out += m_coeffs[i] * m_buf[idx];
    }
    return out;
}

// -----------------------------------------------------------------------
// MovingMaximum
// -----------------------------------------------------------------------
void MovingMaximum::push(double v)
{
    m_buf.push_back(v);
    if ((int)m_buf.size() > m_size)
        m_buf.pop_front();
}

double MovingMaximum::getMaximum() const
{
    if (m_buf.empty()) return 0.0;
    double mx = m_buf[0];
    for (auto x : m_buf) if (x > mx) mx = x;
    return mx;
}

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------
dsc_rx::dsc_rx(int sample_rate, MessageCallback callback,
               const std::string &label)
    : m_sample_rate(sample_rate),
      m_callback(std::move(callback)),
      m_dbg_label(label),
      m_dbg_sample_count(0)
{
    // Validate sample rate
    if (m_sample_rate != 10000) {
        fprintf(stderr, "[DSC-WARN] %s: expected 10000 Hz, got %d Hz\n",
                m_dbg_label.c_str(), m_sample_rate);
    }

    // Build complex exponential table for FREQUENCY_SHIFT/2 = 85 Hz
    // SDRangel uses length 600 at 1000 Hz; we use 6000 at 10000 Hz.
    m_exp.resize(EXP_LENGTH);
    double f0 = 0.0;
    const double step = 2.0 * M_PI * (DSCDEMOD_FREQUENCY_SHIFT / 2.0)
                        / (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * RATE_SCALE);
    for (int i = 0; i < EXP_LENGTH; i++) {
        m_exp[i] = cmplx(std::cos(f0), std::sin(f0));
        f0 += step;
        if (f0 >= 2.0 * M_PI) f0 -= 2.0 * M_PI;
    }
    m_expIdx = 0;

    // FIR lowpass at BAUD_RATE * 1.1 = 110 Hz
    // Normalised cutoff = 110 / (10000) = 0.011
    // 301 taps — same as SDRangel
    const double fc = (DSCDEMOD_BAUD_RATE * 1.1) / (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * RATE_SCALE);
    m_lpfMark.create(301, fc);
    m_lpfSpace.create(301, fc);

    // Moving maximum window = samplesPerBit * 8 (same ratio as SDRangel: 10*8=80 at 1000 Hz)
    m_movMaxMark.setSize(SAMPLES_PER_BIT * 8);
    m_movMaxSpace.setSize(SAMPLES_PER_BIT * 8);

    m_markEnv  = 0.0;
    m_spaceEnv = 0.0;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;

    init();
}

// -----------------------------------------------------------------------
// init — reset DSC decoder state
// Mirrors DSCDemodSink::init() exactly.
// -----------------------------------------------------------------------
void dsc_rx::init()
{
    m_expIdx    = 0;
    m_bits      = 0;
    m_bitCount  = 0;
    m_gotSOP    = false;

    // SDRangel: m_clockCount = -m_samplesPerBit/2.0
    m_clockCount = -(double)SAMPLES_PER_BIT / 2.0;
    m_data       = false;
    m_dataPrev   = false;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;
}

// -----------------------------------------------------------------------
// process — feed CS16 interleaved I/Q samples
// nb_samples is the number of I/Q *pairs*
// -----------------------------------------------------------------------
void dsc_rx::process(const int16_t *iq_data, int nb_samples)
{
    // Scale to [-1, +1] — same as SDRangel's ci /= SDR_RX_SCALEF (32768.0)
    const double scale = 1.0 / 32768.0;

    for (int i = 0; i < nb_samples; i++) {
        double I = (double)iq_data[i * 2    ] * scale;
        double Q = (double)iq_data[i * 2 + 1] * scale;
        processOneSample(cmplx(I, Q));
        m_dbg_sample_count++;
    }
}

// -----------------------------------------------------------------------
// processOneSample — direct translation of DSCDemodSink::processOneSample()
// -----------------------------------------------------------------------
void dsc_rx::processOneSample(cmplx ci)
{
    // RSSI accumulation while receiving (before any scaling — already normalised)
    if (m_gotSOP) {
        double magsq = ci.real() * ci.real() + ci.imag() * ci.imag();
        m_rssiMagSqSum   += magsq;
        m_rssiMagSqCount++;
    }

    // Correlate with complex exponential at ±85 Hz
    // SDRangel: corr1 = ci * exp[idx]  (mark channel)
    //           corr2 = ci * conj(exp[idx])  (space channel)
    cmplx exp_val = m_exp[m_expIdx];
    m_expIdx = (m_expIdx + 1) % EXP_LENGTH;

    cmplx corr1 = ci * exp_val;            // mark  (+85 Hz)
    cmplx corr2 = ci * std::conj(exp_val); // space (-85 Hz)

    // Low-pass filter then take magnitude
    double abs1Filt = std::abs(m_lpfMark.filter(corr1));
    double abs2Filt = std::abs(m_lpfSpace.filter(corr2));

    // Moving maximum envelope
    m_movMaxMark.push(abs1Filt);
    m_movMaxSpace.push(abs2Filt);
    double env1 = m_movMaxMark.getMaximum();
    double env2 = m_movMaxSpace.getMaximum();

    m_markEnv  = env1;
    m_spaceEnv = env2;

    // Automatic Threshold Correction (ATC)
    // http://www.w7ay.net/site/Technical/ATC/index.html
    double bias1 = abs1Filt - 0.5 * env1;
    double bias2 = abs2Filt - 0.5 * env2;
    // double unbiasedData = abs1Filt - abs2Filt;  // (unused, kept for reference)
    double biasedData   = bias1 - bias2;

    // Bit decision
    m_dataPrev = m_data;
    m_data     = (biasedData > 0.0);

    // Clock recovery — SDRangel: only on RISING EDGE (0→1)
    // "if (m_data && !m_dataPrev)"
    if (m_data && !m_dataPrev) {
        m_clockCount -= m_clockCount * 0.25;
    }

    // Advance clock; sample bit in middle of bit period
    // SDRangel: "if (m_clockCount >= m_samplesPerBit/2.0 - 1.0)"
    m_clockCount += 1.0;
    if (m_clockCount >= (double)SAMPLES_PER_BIT / 2.0 - 1.0) {
        receiveBit(m_data);
        m_clockCount -= (double)SAMPLES_PER_BIT;
    }
}

// -----------------------------------------------------------------------
// receiveBit — direct translation of DSCDemodSink::receiveBit()
// -----------------------------------------------------------------------
void dsc_rx::receiveBit(bool bit)
{
    // Store in shift register (MSB first, same as SDRangel)
    m_bits = (m_bits << 1) | (bit ? 1u : 0u);
    m_bitCount++;

    if (!m_gotSOP)
    {
        // SDRangel: "if (m_bitCount == 10*3)"
        if (m_bitCount == 10 * 3)
        {
            // Slide window by 1 bit
            m_bitCount--;

            unsigned int pat = m_bits & 0x3FFFFFFFu;

            // Exact match only — same as SDRangel
            for (int i = 0; i < DSCDecoder::m_phasingPatternsSize; i++)
            {
                if (pat == DSCDecoder::m_phasingPatterns[i].m_pattern)
                {
                    m_dscDecoder.init(DSCDecoder::m_phasingPatterns[i].m_offset);
                    m_gotSOP   = true;
                    m_bitCount = 0;
                    m_rssiMagSqSum   = 0.0;
                    m_rssiMagSqCount = 0;
                    break;
                }
            }
        }
    }
    else
    {
        // SDRangel: "if (m_bitCount == 10)"
        if (m_bitCount == 10)
        {
            if (m_dscDecoder.decodeBits(m_bits & 0x3FFu))
            {
                // Message complete
                std::vector<unsigned char> bytes = m_dscDecoder.getMessage();
                int errors = m_dscDecoder.getErrors();

                float rssi = -100.0f;
                if (m_rssiMagSqCount > 0) {
                    double avgMagSq = m_rssiMagSqSum / (double)m_rssiMagSqCount;
                    if (avgMagSq > 0.0)
                        rssi = static_cast<float>(10.0 * std::log10(avgMagSq));
                }

                time_t now = time(nullptr);
                DSCMessage message(bytes, now);

                if (m_callback)
                    m_callback(message, errors, rssi);

                // Reset demod — SDRangel calls init() here
                init();
            }
            m_bitCount = 0;
        }
    }
}

// -----------------------------------------------------------------------
// getStats
// -----------------------------------------------------------------------
dsc_rx::DecoderStats dsc_rx::getStats() const
{
    DecoderStats s;
    s.signal_level = m_markEnv - m_spaceEnv;
    s.mark_level   = m_markEnv;
    s.space_level  = m_spaceEnv;
    s.receiving    = m_gotSOP;
    s.bit_count    = m_bitCount;

    if (m_gotSOP && m_rssiMagSqCount > 0) {
        double avgMagSq = m_rssiMagSqSum / (double)m_rssiMagSqCount;
        s.rssi_db = (avgMagSq > 0.0) ? 10.0 * std::log10(avgMagSq) : -100.0;
    } else {
        s.rssi_db = -100.0;
    }

    return s;
}
