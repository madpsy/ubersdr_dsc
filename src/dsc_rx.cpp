/* -*- c++ -*- */
/*
 * dsc_rx.cpp — DSC (Digital Selective Calling) FSK receiver
 *
 * Demodulator ported from SDRangel DSCDemodSink (Jon Beniston, M7RCE):
 *   Copyright (C) 2023 Jon Beniston, M7RCE
 *
 * SDRangel operates at DSCDEMOD_CHANNEL_SAMPLE_RATE = 1000 Hz internally.
 * ubersdr delivers IQ at whatever its [iq] preset says -- 12 kHz today, 10 kHz
 * when this was written -- so every sample-count constant is multiplied by
 * rateScale = sample_rate / 1000, taken from the rate the receiver is
 * constructed with.  These used to be file-scope constants fixed at a scale of
 * 10, which meant a 12 kHz stream was demodulated with 10 kHz tone spacing,
 * filter cutoffs and symbol timing: 20% wrong everywhere, and silent about it.
 *
 * The algorithm is a direct translation of dscdemodsink.cpp using the
 * SDRangel FirFilter / Lowpass<Complex> template from firfilter.h:
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

// -----------------------------------------------------------------------
// Rate-derived configuration
// -----------------------------------------------------------------------
//
// WHY AN INTEGER SCALE, AND WHY A RATE THAT DOES NOT DIVIDE IS REFUSED
//
// Two things have to come out exact, and neither is merely tidiness:
//
//   The exponential table must hold a whole number of cycles of the 85 Hz
//   tone, because processOneSample walks it with `(idx + 1) % exp_length` and
//   starts over.  At 1000 Hz, 600 samples carry exactly 51 cycles; multiplying
//   both the length and the rate by the same integer keeps that exact.  A
//   fractional scale would leave the table not closing, and the local
//   oscillator would step in phase once per wrap -- a defect that looks like
//   sporadic bit errors rather than anything obviously wrong.
//
//   samples_per_bit must be a whole number of samples, since it also sizes the
//   moving-maximum window.
//
// Both hold exactly when the rate is an integer multiple of 1000 Hz, which is
// every rate ubersdr can deliver here: radiod's presets are expressed in whole
// kHz (presets.conf [iq] is samprate = 12k).  So the alternative -- adding a
// resampler -- would buy nothing real, while putting a filter of our own design
// in front of a demodulator whose whole value is being a checkable
// transliteration of SDRangel's.  A rate that does not divide is therefore
// refused rather than resampled or rounded: the receiver says so and then
// discards samples, because a mistuned DSC decoder emits plausible traffic
// rather than nothing, and this is a distress-calling band.
dsc_rx::RateConfig dsc_rx::rateConfigFor(int sample_rate)
{
    RateConfig c;
    c.sample_rate     = sample_rate;
    c.lpf_cutoff_hz   = DSCDEMOD_BAUD_RATE * 1.1;   // 110 Hz, an absolute frequency
    c.supported       = (sample_rate > 0)
                        && (sample_rate % DSCDEMOD_CHANNEL_SAMPLE_RATE == 0);

    if (!c.supported) {
        c.rate_scale = 0; c.exp_length = 0; c.samples_per_bit = 0;
        c.movmax_window = 0; c.phase_step = 0.0; c.lpf_sample_rate = 0.0;
        return c;
    }

    c.rate_scale      = sample_rate / DSCDEMOD_CHANNEL_SAMPLE_RATE;
    c.exp_length      = DSCDEMOD_EXP_LENGTH * c.rate_scale;
    c.samples_per_bit = (DSCDEMOD_CHANNEL_SAMPLE_RATE * c.rate_scale)
                        / DSCDEMOD_BAUD_RATE;
    c.movmax_window   = c.samples_per_bit * 8;
    c.phase_step      = 2.0 * M_PI * (DSCDEMOD_FREQUENCY_SHIFT / 2.0)
                        / (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * c.rate_scale);
    c.lpf_sample_rate = (double)(DSCDEMOD_CHANNEL_SAMPLE_RATE * c.rate_scale);
    return c;
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
      m_rate(rateConfigFor(sample_rate)),
      m_callback(std::move(callback)),
      m_dbg_label(label),
      m_dbg_sample_count(0)
{
    // A rate this demodulator cannot be built for.  Say so once and then decode
    // nothing, rather than decoding it wrong -- see rateConfigFor.
    if (!m_rate.supported) {
        fprintf(stderr,
                "[DSC-WARN] %s: %d Hz is not an integer multiple of %d Hz; "
                "this channel will not be demodulated\n",
                m_dbg_label.c_str(), m_sample_rate, DSCDEMOD_CHANNEL_SAMPLE_RATE);
        m_markEnv = m_spaceEnv = 0.0;
        m_rssiMagSqSum = 0.0;
        m_rssiMagSqCount = 0;
        m_expIdx = 0;
        m_bits = 0; m_bitCount = 0; m_gotSOP = false;
        m_clockCount = 0.0; m_data = false; m_dataPrev = false;
        return;
    }

    // Build complex exponential table for FREQUENCY_SHIFT/2 = 85 Hz
    // SDRangel uses length 600 at 1000 Hz; we use 600 * rate_scale here.
    m_exp.resize(m_rate.exp_length);
    double f0 = 0.0;
    const double step = m_rate.phase_step;
    for (int i = 0; i < m_rate.exp_length; i++) {
        m_exp[i] = Complex(std::cos(f0), std::sin(f0));
        f0 += step;
        if (f0 >= 2.0 * M_PI) f0 -= 2.0 * M_PI;
    }
    m_expIdx = 0;

    // FIR lowpass — exact match to SDRangel:
    //   m_lowpassComplex1.create(301, DSCDEMOD_CHANNEL_SAMPLE_RATE, DSCDEMOD_BAUD_RATE * 1.1)
    //   = create(301, 1000, 110)
    // We pass the same absolute Hz values scaled to our sample rate:
    //   create(301, sample_rate, 110)
    // The cutoff in Hz is the same (110 Hz); only sampleRate changes, which is
    // exactly what was wrong before -- it was always told 10000.
    m_lpfMark.create(301, m_rate.lpf_sample_rate, m_rate.lpf_cutoff_hz);
    m_lpfSpace.create(301, m_rate.lpf_sample_rate, m_rate.lpf_cutoff_hz);

    // Moving maximum window = samplesPerBit * 8 (same ratio as SDRangel: 10*8=80 at 1000 Hz)
    m_movMaxMark.setSize(m_rate.movmax_window);
    m_movMaxSpace.setSize(m_rate.movmax_window);

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
    m_clockCount = -(double)m_rate.samples_per_bit / 2.0;
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
    if (!m_rate.supported) return;   // warned at construction

    // Scale to [-1, +1] — same as SDRangel's ci /= SDR_RX_SCALEF (32768.0)
    const double scale = 1.0 / 32768.0;

    for (int i = 0; i < nb_samples; i++) {
        double I = (double)iq_data[i * 2    ] * scale;
        double Q = (double)iq_data[i * 2 + 1] * scale;
        processOneSample(Complex(I, Q));
        m_dbg_sample_count++;
    }

    // One diagnostic line per channel every 30 s at the rate actually in use.
    // Diagnosis:
    //   mark=0 space=0  → no IQ arriving, or exp table wrong
    //   mark>0 space>0  → signal present, ATC working
    //   diff≈0          → no FSK visible (wrong frequency or no signal)
    //   diff≠0          → FSK visible, phasing should eventually lock
    const long long DIAG_INTERVAL = 30LL * (long long)m_sample_rate;
    long long prev = m_dbg_sample_count - (long long)nb_samples;
    if (m_dbg_sample_count / DIAG_INTERVAL != prev / DIAG_INTERVAL) {
        fprintf(stderr,
            "[DSC] %-14s  t=%llds  mark=%.4f  space=%.4f  diff=%.4f  sop=%s\n",
            m_dbg_label.c_str(),
            (long long)(m_dbg_sample_count / m_sample_rate),
            m_markEnv, m_spaceEnv,
            m_markEnv - m_spaceEnv,
            m_gotSOP ? "YES" : "no");
        fflush(stderr);
    }
}

// -----------------------------------------------------------------------
// processOneSample — direct translation of DSCDemodSink::processOneSample()
// -----------------------------------------------------------------------
void dsc_rx::processOneSample(Complex ci)
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
    Complex exp_val = m_exp[m_expIdx];
    m_expIdx = (m_expIdx + 1) % m_rate.exp_length;

    Complex corr1 = ci * exp_val;                        // mark  (+85 Hz)
    Complex corr2 = ci * std::conj(exp_val);             // space (-85 Hz)

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
    if (m_clockCount >= (double)m_rate.samples_per_bit / 2.0 - 1.0) {
        receiveBit(m_data);
        m_clockCount -= (double)m_rate.samples_per_bit;
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
