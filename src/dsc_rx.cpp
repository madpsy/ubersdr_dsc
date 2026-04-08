/* -*- c++ -*- */
/*
 * dsc_rx.cpp — DSC (Digital Selective Calling) FSK receiver
 *
 * Demodulator ported from SDRangel DSCDemodSink (Jon Beniston, M7RCE):
 *   Copyright (C) 2023 Jon Beniston, M7RCE
 *
 * DSC framing adapted from SDRangel DSCDemodSink.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Architecture:
 *   Input: CS16 interleaved I/Q at 10000 Hz from ubersdr (mode=iq)
 *
 *   For each sample:
 *   1. Correlate with exp(+j·2π·85·t/Fs) → mark channel (mark tone at +85 Hz)
 *      Correlate with conj(exp)           → space channel (space tone at -85 Hz)
 *   2. FIR lowpass at 110 Hz on each channel
 *   3. abs() → envelope
 *   4. Moving maximum over 8 bit periods → env1, env2
 *   5. ATC: bias = abs - 0.5 * env
 *   6. biasedData = bias_mark - bias_space  (positive = mark = 1)
 *   7. Edge-triggered clock: on any bit transition, pull clock 25% toward zero
 *   8. Sample bit in middle of bit period
 *   9. Feed bit into DSC phasing/symbol decoder
 */

#include "dsc_rx.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <algorithm>

// DSC parameters
static const double DSC_BAUD_RATE       = 100.0;
static const double DSC_FREQ_SHIFT      = 170.0;   // Hz total shift
static const double DSC_HALF_SHIFT      = DSC_FREQ_SHIFT / 2.0;  // 85 Hz

// Debug intervals (in samples at 10000 Hz)
static const long long DBG_STATS_INTERVAL    = 30LL * 10000;
static const long long DBG_NEARMISS_INTERVAL = 10LL * 10000;

// -----------------------------------------------------------------------
// SimpleLowpass — windowed-sinc FIR lowpass for complex samples
// -----------------------------------------------------------------------
void SimpleLowpass::create(int taps, double fc)
{
    // taps must be odd
    if (taps % 2 == 0) taps++;
    m_coeffs.resize(taps);
    int half = taps / 2;
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        int n = i - half;
        double sinc = (n == 0) ? 1.0 : std::sin(M_PI * n * 2.0 * fc) / (M_PI * n * 2.0 * fc);
        // Blackman window
        double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * i / (taps - 1))
                        + 0.08 * std::cos(4.0 * M_PI * i / (taps - 1));
        m_coeffs[i] = sinc * w;
        sum += m_coeffs[i];
    }
    // Normalise
    for (auto &c : m_coeffs) c /= sum;
    // Initialise delay line
    m_buf.assign(taps, cmplx(0.0, 0.0));
}

cmplx SimpleLowpass::filter(cmplx in)
{
    m_buf.push_front(in);
    m_buf.pop_back();
    cmplx out(0.0, 0.0);
    for (size_t i = 0; i < m_coeffs.size(); i++)
        out += m_coeffs[i] * m_buf[i];
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
// Hamming distance between two 30-bit words
// -----------------------------------------------------------------------
int dsc_rx::hamming30(unsigned int a, unsigned int b)
{
    unsigned int x = (a ^ b) & 0x3FFFFFFFu;
    int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------
dsc_rx::dsc_rx(int sample_rate, MessageCallback callback,
               const std::string &label)
    : m_sample_rate(sample_rate),
      m_callback(std::move(callback)),
      m_dbg_label(label),
      m_dbg_sample_count(0),
      m_dbg_total_bits(0),
      m_dbg_near_miss_sample(-1000000LL),
      m_dbg_stats_sample(0)
{
    m_baud_rate       = DSC_BAUD_RATE;
    m_bit_sample_count = (double)m_sample_rate / m_baud_rate;

    // Build complex exponential table for half-shift frequency (85 Hz).
    // Period = sample_rate / gcd(sample_rate, 85).
    // For 10000 Hz: gcd(10000, 85) = 5, period = 2000.
    // We use a period of sample_rate (10000) for simplicity — it wraps correctly.
    int exp_len = m_sample_rate;  // one second of table
    m_exp.resize(exp_len);
    double f0 = 0.0;
    const double step = 2.0 * M_PI * DSC_HALF_SHIFT / (double)m_sample_rate;
    for (int i = 0; i < exp_len; i++) {
        m_exp[i] = cmplx(std::cos(f0), std::sin(f0));
        f0 += step;
        if (f0 >= 2.0 * M_PI) f0 -= 2.0 * M_PI;
    }
    m_expIdx = 0;

    // FIR lowpass at baud_rate * 1.1 = 110 Hz
    // Normalised cutoff = 110 / sample_rate = 110 / 10000 = 0.011
    // Use 301 taps (same as SDRangel)
    double fc = (m_baud_rate * 1.1) / (double)m_sample_rate;
    m_lpfMark.create(301, fc);
    m_lpfSpace.create(301, fc);

    // Moving maximum window = 8 bit periods
    int movmax_size = static_cast<int>(m_bit_sample_count * 8.0);
    m_movMaxMark.setSize(movmax_size);
    m_movMaxSpace.setSize(movmax_size);

    m_markEnv  = 0.0;
    m_spaceEnv = 0.0;

    // Clock: initialise to -half_bit so first sample fires after one full bit
    m_clockCount = -m_bit_sample_count / 2.0;
    m_data     = false;
    m_dataPrev = false;

    // DSC state
    m_bits     = 0;
    m_bitCount = 0;
    m_gotSOP   = false;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;
}

// -----------------------------------------------------------------------
// init — reset DSC decoder state (called after message complete or on error)
// -----------------------------------------------------------------------
void dsc_rx::init()
{
    m_bits     = 0;
    m_bitCount = 0;
    m_gotSOP   = false;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;

    // Reset clock
    m_clockCount = -m_bit_sample_count / 2.0;
    m_data     = false;
    m_dataPrev = false;
}

// -----------------------------------------------------------------------
// process — feed CS16 interleaved I/Q samples
// nb_samples is the number of I/Q *pairs* (not raw int16 count)
// -----------------------------------------------------------------------
void dsc_rx::process(const int16_t * iq_data, int nb_samples)
{
    const double scale = 1.0 / 32768.0;

    for (int i = 0; i < nb_samples; i++) {
        // CS16: little-endian, interleaved I then Q
        double I = (double)iq_data[i * 2    ] * scale;
        double Q = (double)iq_data[i * 2 + 1] * scale;
        cmplx ci(I, Q);
        processOneSample(ci);
    }
}

// -----------------------------------------------------------------------
// processOneSample — SDRangel-style IQ FSK demodulation
// -----------------------------------------------------------------------
void dsc_rx::processOneSample(cmplx ci)
{
    // 1. Correlate with complex exponential at ±85 Hz
    cmplx exp_val = m_exp[m_expIdx];
    m_expIdx = (m_expIdx + 1) % (int)m_exp.size();

    cmplx corr1 = ci * exp_val;           // mark channel (+85 Hz)
    cmplx corr2 = ci * std::conj(exp_val); // space channel (-85 Hz)

    // 2. FIR lowpass filter
    double abs1 = std::abs(m_lpfMark.filter(corr1));
    double abs2 = std::abs(m_lpfSpace.filter(corr2));

    // 3. Moving maximum envelope
    m_movMaxMark.push(abs1);
    m_movMaxSpace.push(abs2);
    double env1 = m_movMaxMark.getMaximum();
    double env2 = m_movMaxSpace.getMaximum();

    m_markEnv  = env1;
    m_spaceEnv = env2;

    // 4. ATC: subtract half the envelope from each channel
    double bias1 = abs1 - 0.5 * env1;
    double bias2 = abs2 - 0.5 * env2;
    double biasedData = bias1 - bias2;  // positive = mark = 1

    // 5. RSSI accumulation while receiving
    if (m_gotSOP) {
        double magsq = ci.real() * ci.real() + ci.imag() * ci.imag();
        m_rssiMagSqSum += magsq;
        m_rssiMagSqCount++;
    }

    // 6. Bit decision
    m_dataPrev = m_data;
    m_data     = (biasedData > 0.0);

    // 7. Edge-triggered clock recovery: on any bit transition, pull 25% toward zero
    if (m_data != m_dataPrev) {
        m_clockCount -= m_clockCount * 0.25;
    }

    // 8. Advance clock; sample bit in middle of bit period
    m_clockCount += 1.0;
    if (m_clockCount >= m_bit_sample_count / 2.0 - 1.0) {
        m_dbg_total_bits++;
        receiveBit(m_data);
        m_clockCount -= m_bit_sample_count;
    }

    m_dbg_sample_count++;

    // Periodic stats dump — one line per channel every 30 s
    long long stats_interval = (long long)(DBG_STATS_INTERVAL /
                                           10000.0 * m_sample_rate);
    if (m_dbg_sample_count - m_dbg_stats_sample >= stats_interval) {
        m_dbg_stats_sample = m_dbg_sample_count;
        fprintf(stderr,
            "[DSC-DBG] %-14s  mark_env=%.3f  space_env=%.3f"
            "  clock=%.1f  bits=%lld  sop=%s\n",
            m_dbg_label.c_str(),
            m_markEnv, m_spaceEnv,
            m_clockCount,
            m_dbg_total_bits,
            m_gotSOP ? "YES" : "no");
    }
}

// -----------------------------------------------------------------------
// receiveBit — DSC phasing detection and symbol decoding
// Adapted from SDRangel DSCDemodSink::receiveBit()
// -----------------------------------------------------------------------
void dsc_rx::receiveBit(bool bit)
{
    m_bits = (m_bits << 1) | (bit ? 1u : 0u);
    m_bitCount++;

    if (!m_gotSOP) {
        // Phasing detection: need 30 bits (3 × 10-bit symbols)
        if (m_bitCount >= 10 * 3) {
            m_bitCount = 10 * 3 - 1;  // slide by 1 bit each call

            unsigned int pat = m_bits & 0x3FFFFFFFu;

            // Find best-matching phasing pattern
            int best_dist = 31;
            int best_idx  = -1;
            for (int i = 0; i < DSCDecoder::m_phasingPatternsSize; i++) {
                int d = hamming30(pat, DSCDecoder::m_phasingPatterns[i].m_pattern);
                if (d < best_dist) { best_dist = d; best_idx = i; }
                if (d == 0) break;
            }

            if (best_dist == 0) {
                // Exact phasing lock
                m_dscDecoder.init(DSCDecoder::m_phasingPatterns[best_idx].m_offset);
                m_gotSOP   = true;
                m_bitCount = 0;
                m_rssiMagSqSum   = 0.0;
                m_rssiMagSqCount = 0;

                fprintf(stderr,
                    "[DSC-DBG] %-14s  PHASING LOCK  pat[%d]=0x%08X"
                    "  offset=%d  bits=%lld\n",
                    m_dbg_label.c_str(), best_idx,
                    DSCDecoder::m_phasingPatterns[best_idx].m_pattern,
                    DSCDecoder::m_phasingPatterns[best_idx].m_offset,
                    m_dbg_total_bits);

            } else if (best_dist <= 2) {
                // Near-miss — rate-limited to once per 10 s
                long long nm_interval = (long long)(DBG_NEARMISS_INTERVAL /
                                                    10000.0 * m_sample_rate);
                if (m_dbg_sample_count - m_dbg_near_miss_sample >= nm_interval) {
                    m_dbg_near_miss_sample = m_dbg_sample_count;
                    fprintf(stderr,
                        "[DSC-DBG] %-14s  near-miss  got=0x%08X"
                        "  best=0x%08X  dist=%d  bits=%lld\n",
                        m_dbg_label.c_str(), pat,
                        DSCDecoder::m_phasingPatterns[best_idx].m_pattern,
                        best_dist, m_dbg_total_bits);
                }
            }
        }
    } else {
        // Symbol decoding: every 10 bits
        if (m_bitCount == 10) {
            if (m_dscDecoder.decodeBits(m_bits & 0x3FFu)) {
                // Message complete
                std::vector<unsigned char> bytes = m_dscDecoder.getMessage();
                int errors = m_dscDecoder.getErrors();

                float rssi = -100.0f;
                if (m_rssiMagSqCount > 0) {
                    double avgMagSq = m_rssiMagSqSum / m_rssiMagSqCount;
                    if (avgMagSq > 0.0)
                        rssi = static_cast<float>(10.0 * std::log10(avgMagSq));
                }

                time_t now = time(nullptr);
                DSCMessage message(bytes, now);

                if (m_callback)
                    m_callback(message, errors, rssi);

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
        double avgMagSq = m_rssiMagSqSum / m_rssiMagSqCount;
        s.rssi_db = (avgMagSq > 0.0) ? 10.0 * std::log10(avgMagSq) : -100.0;
    } else {
        s.rssi_db = -100.0;
    }

    return s;
}
