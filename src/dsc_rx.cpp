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
 * FIR filter: exact port of SDRangel's generateLowPassFilter() +
 * FirFilter<Complex>::filter() — uses half-tap symmetry optimisation,
 * absolute Hz cutoff (not normalised), and SDRangel's Blackman window.
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
// SimpleLowpass — exact port of SDRangel's generateLowPassFilter() +
// FirFilter<Complex>::filter().
//
// SDRangel stores only the half-taps (nTaps/2 + 1 entries) and exploits
// the symmetry of a linear-phase FIR:
//   acc += (samples[a] + samples[b]) * taps[i]   for i = 0..n_taps-1
//   acc += samples[a] * taps[n_taps]              (centre tap)
//
// The tap generator (firfilter.cpp::generateLowPassFilter) uses:
//   Wc = 2π * cutoff / sampleRate
//   taps[i] = sin(n * Wc) / (n * π)   (sinc, n = i - halfTaps + 1)
//   taps[halfTaps-1] = Wc / π          (centre tap, n=0)
//   Blackman: 0.42 + 0.5*cos(2πn/nTaps) + 0.08*cos(4πn/nTaps)
//   Normalise: sum = 2 * Σ(taps[0..halfTaps-2]) + taps[halfTaps-1]
// -----------------------------------------------------------------------
void SimpleLowpass::create(int nTaps, double sampleRate, double cutoff)
{
    // nTaps must be odd
    if (!(nTaps & 1)) nTaps++;

    double Wc       = (2.0 * M_PI * cutoff) / sampleRate;
    int    halfTaps = nTaps / 2 + 1;   // number of tap entries stored

    m_taps.resize(halfTaps);

    for (int i = 0; i < halfTaps; ++i)
    {
        if (i == halfTaps - 1)
        {
            // Centre tap (n = 0): sinc(0) = Wc/π
            m_taps[i] = Wc / M_PI;
        }
        else
        {
            int n = i - (nTaps - 1) / 2;   // n is negative for i < halfTaps-1
            m_taps[i] = std::sin(n * Wc) / (n * M_PI);
        }
    }

    // Blackman window — SDRangel: 0.42 + 0.5*cos(2πn/nTaps) + 0.08*cos(4πn/nTaps)
    for (int i = 0; i < halfTaps; ++i)
    {
        int n = i - (nTaps - 1) / 2;
        m_taps[i] *= 0.42 + 0.5  * std::cos((2.0 * M_PI * n) / nTaps)
                          + 0.08 * std::cos((4.0 * M_PI * n) / nTaps);
    }

    // Normalise: full filter sum = 2 * Σ(half taps except centre) + centre
    double sum = 0.0;
    for (int i = 0; i < halfTaps - 1; ++i)
        sum += m_taps[i] * 2.0;
    sum += m_taps[halfTaps - 1];

    for (auto &t : m_taps) t /= sum;

    // Circular buffer — same size as tap storage
    m_buf.assign(halfTaps, cmplx(0.0, 0.0));
    m_ptr = 0;
}

cmplx SimpleLowpass::filter(cmplx in)
{
    // Exact port of SDRangel FirFilter<Complex>::filter()
    // m_taps has halfTaps = nTaps/2 + 1 entries
    // m_buf  has halfTaps entries (circular)
    unsigned int n_samples = (unsigned int)m_buf.size();
    unsigned int n_taps    = (unsigned int)m_taps.size() - 1;  // last tap is centre

    // Write new sample
    m_buf[m_ptr] = in;

    unsigned int a = m_ptr;
    unsigned int b = (a == n_samples - 1) ? 0 : a + 1;

    cmplx acc(0.0, 0.0);

    for (unsigned int i = 0; i < n_taps; ++i)
    {
        acc += (m_buf[a] + m_buf[b]) * m_taps[i];

        a = (a == 0)             ? n_samples - 1 : a - 1;
        b = (b == n_samples - 1) ? 0             : b + 1;
    }

    // Centre tap
    acc += m_buf[a] * m_taps[n_taps];

    // Advance write pointer
    m_ptr = (m_ptr == n_samples - 1) ? 0 : m_ptr + 1;

    return acc;
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

    // FIR lowpass — exact match to SDRangel:
    //   m_lowpassComplex1.create(301, DSCDEMOD_CHANNEL_SAMPLE_RATE, DSCDEMOD_BAUD_RATE * 1.1)
    //   = create(301, 1000, 110)
    // We pass the same absolute Hz values scaled to our sample rate:
    //   create(301, 10000, 110)
    // The cutoff in Hz is the same (110 Hz); only sampleRate changes.
    const double cutoff_hz = DSCDEMOD_BAUD_RATE * 1.1;  // 110 Hz
    m_lpfMark.create(301, (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * RATE_SCALE), cutoff_hz);
    m_lpfSpace.create(301, (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * RATE_SCALE), cutoff_hz);

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
