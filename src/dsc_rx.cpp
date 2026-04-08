/* -*- c++ -*- */
/*
 * dsc_rx.cpp — DSC (Digital Selective Calling) FSK receiver
 *
 * FSK demodulator adapted from navtex_rx.cpp:
 *   Copyright 2020 Franco Venturi
 *   Copyright (C) 2011-2016 Remi Chateauneu, F4ECW
 *   Copyright (C) Rik van Riel, AB1KW
 *   Copyright (C) Paul Lutus (JNX)
 *
 * DSC framing adapted from SDRangel dscdemodsink.cpp:
 *   Copyright (C) 2023 Jon Beniston, M7RCE
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// -----------------------------------------------------------------------
// Architecture overview
//
// The FSK demodulator is taken directly from navtex_rx.cpp:
//   1. Mark/space bandpass filters (FFT overlap-add via fftfilt)
//   2. Envelope detection with fast-attack / slow-decay averaging
//   3. Automatic threshold correction (W7AY ATC algorithm)
//   4. Multicorrelator bit synchronization (early/prompt/late)
//
// DSC and NAVTEX share identical RF parameters:
//   - 100 baud, 170 Hz shift, center frequency 500 Hz
//
// The bit-level processing replaces NAVTEX's 7-bit CCIR 476 / SITOR-B
// framing with DSC's 10-bit symbol framing:
//   - Before phasing: check every bit against 30-bit phasing patterns
//     (3 consecutive 10-bit symbols from DSCDecoder::m_phasingPatterns)
//   - After phasing: every 10 bits, feed into DSCDecoder::decodeBits()
//   - When DSCDecoder signals message complete, construct DSCMessage
//     and invoke the callback
// -----------------------------------------------------------------------

#include "dsc_rx.h"
#include "fftfilt.h"
#include "misc.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>

// DSC / NAVTEX share these RF parameters
static const int    deviation_f       = 85;       // ±85 Hz = 170 Hz shift
static const double dflt_center_freq  = 500.0;    // audio center frequency

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------
// Periodic stats interval: every 30 seconds worth of samples
static const long long DBG_STATS_INTERVAL  = 30LL * 12000;  // scaled at runtime
// Near-miss rate limit: at most once per 10 seconds worth of samples
static const long long DBG_NEARMISS_INTERVAL = 10LL * 12000; // scaled at runtime

// Hamming distance between two 30-bit words
static int hamming30(unsigned int a, unsigned int b)
{
    unsigned int x = (a ^ b) & 0x3FFFFFFFu;
    // popcount via Brian Kernighan
    int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

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
    m_center_frequency_f = dflt_center_freq;
    m_baud_rate = 100.0;

    double bit_duration_seconds = 1.0 / m_baud_rate;
    m_bit_sample_count = m_sample_rate * bit_duration_seconds;

    // Edge-triggered clock — initialise to -half_bit so the first sample
    // fires after one full bit period (matches SDRangel init())
    m_clockCount = -m_bit_sample_count / 2.0;
    m_data     = false;
    m_dataPrev = false;

    // Envelope trackers — start at zero, fast-attack will ramp up quickly
    m_mark_env  = 0;
    m_space_env = 0;

    // DSC-specific state
    m_bits     = 0;
    m_bitCount = 0;
    m_gotSOP   = false;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;

    // Filters
    m_mark_lowpass  = nullptr;
    m_space_lowpass = nullptr;

    set_filter_values();
    configure_filters();
}

// -----------------------------------------------------------------------
// Destructor
// -----------------------------------------------------------------------
dsc_rx::~dsc_rx()
{
    delete m_mark_lowpass;
    delete m_space_lowpass;
}

// -----------------------------------------------------------------------
// Reset decoder state (called after a message completes or on error)
// -----------------------------------------------------------------------
void dsc_rx::init()
{
    m_bits     = 0;
    m_bitCount = 0;
    m_gotSOP   = false;

    m_rssiMagSqSum   = 0.0;
    m_rssiMagSqCount = 0;

    // Reset clock so the next bit fires after one full bit period
    m_clockCount = -m_bit_sample_count / 2.0;
    m_data     = false;
    m_dataPrev = false;
}

// -----------------------------------------------------------------------
// Filter setup — identical to navtex_rx
// -----------------------------------------------------------------------
void dsc_rx::set_filter_values()
{
    m_mark_f     = m_center_frequency_f + deviation_f;
    m_space_f    = m_center_frequency_f - deviation_f;
    m_mark_phase  = 0;
    m_space_phase = 0;
}

void dsc_rx::configure_filters()
{
    const int filtlen = 512;

    delete m_mark_lowpass;
    m_mark_lowpass = new fftfilt(m_baud_rate / m_sample_rate, filtlen);
    m_mark_lowpass->rtty_filter(m_baud_rate / m_sample_rate);

    delete m_space_lowpass;
    m_space_lowpass = new fftfilt(m_baud_rate / m_sample_rate, filtlen);
    m_space_lowpass->rtty_filter(m_baud_rate / m_sample_rate);
}

// -----------------------------------------------------------------------
// Public process() — feed audio samples
// -----------------------------------------------------------------------
void dsc_rx::process(const float * data, int nb_samples)
{
    cmplx z, zmark, zspace, *zp_mark, *zp_space;

    for (int i = 0; i < nb_samples; i++) {
        // Scale float [-1,1] to int16 range, same as navtex_rx
        double dv = 32767.0 * data[i];
        z = cmplx(dv, 0);

        zmark = mixer(m_mark_phase, m_mark_f, z);
        m_mark_lowpass->run(zmark, &zp_mark);

        zspace = mixer(m_space_phase, m_space_f, z);
        int n_out = m_space_lowpass->run(zspace, &zp_space);

        if (n_out)
            process_fft_output(zp_mark, zp_space, n_out);
    }
}

void dsc_rx::process(const int16_t * data, int nb_samples)
{
    cmplx z, zmark, zspace, *zp_mark, *zp_space;

    for (int i = 0; i < nb_samples; i++) {
        z = cmplx(data[i], 0);

        zmark = mixer(m_mark_phase, m_mark_f, z);
        m_mark_lowpass->run(zmark, &zp_mark);

        zspace = mixer(m_space_phase, m_space_f, z);
        int n_out = m_space_lowpass->run(zspace, &zp_space);

        if (n_out)
            process_fft_output(zp_mark, zp_space, n_out);
    }
}

// -----------------------------------------------------------------------
// Mixer — frequency shift via complex multiply (from navtex_rx)
// -----------------------------------------------------------------------
cmplx dsc_rx::mixer(double & phase, double f, cmplx in)
{
    cmplx z = cmplx(cos(phase), sin(phase)) * in;

    phase -= 2.0 * M_PI * f / m_sample_rate;
    if (phase < -2.0 * M_PI)
        phase += 2.0 * M_PI;

    return z;
}

// -----------------------------------------------------------------------
// Process FFT filter output — envelope detection + ATC + bit sampling
// Adapted from navtex_rx::process_fft_output() with per-instance state
// -----------------------------------------------------------------------
void dsc_rx::process_fft_output(cmplx * zp_mark, cmplx * zp_space, int samples)
{
    for (int i = 0; i < samples; i++) {
        double mark_abs  = std::abs(zp_mark[i]);
        double space_abs = std::abs(zp_space[i]);

        // Update envelope trackers (fast attack, slow decay)
        m_mark_env  = envelope_decay(m_mark_env,  mark_abs);
        m_space_env = envelope_decay(m_space_env, space_abs);

        // Clip to envelope (prevent outliers from skewing the ATC)
        mark_abs  = std::min(mark_abs, m_mark_env);
        space_abs = std::min(space_abs, m_space_env);

        // Mark-space discriminator with automatic threshold correction.
        // Uses the SDRangel simplified ATC: subtract half the envelope from
        // each channel before comparing.  This is more robust than the full
        // W7AY quadratic formula when the mark/space envelopes are unbalanced
        // (common on HF due to selective fading), which can cause the quadratic
        // term to dominate and invert the bit decision.
        // http://www.w7ay.net/site/Technical/ATC/
        double bias_mark  = mark_abs  - 0.5 * m_mark_env;
        double bias_space = space_abs - 0.5 * m_space_env;
        double logic_level = bias_mark - bias_space;

        // Demodulated bit decision: positive logic_level = mark = 1
        m_dataPrev = m_data;
        m_data     = (logic_level > 0.0);

        // Accumulate signal magnitude for RSSI while receiving
        if (m_gotSOP) {
            double magsq = mark_abs * mark_abs + space_abs * space_abs;
            m_rssiMagSqSum += magsq;
            m_rssiMagSqCount++;
        }

        // ---------------------------------------------------------------
        // Edge-triggered clock recovery (SDRangel style)
        //
        // On each 0→1 data transition we expect m_clockCount to be near
        // zero (start of a new bit).  Pull the clock 25% toward zero to
        // track the signal timing.  Then advance the clock by one sample;
        // when it reaches samplesPerBit/2 - 1 we are in the middle of the
        // bit — sample it and wrap the counter back by one full bit period.
        // ---------------------------------------------------------------
        // Correct clock on any bit transition (both rising and falling edges).
        // At 120 samples/bit, correcting only on rising edges leaves too much
        // drift during long same-bit runs; both edges halve the maximum drift.
        if (m_data != m_dataPrev) {
            m_clockCount -= m_clockCount * 0.25;
        }

        m_clockCount += 1.0;
        if (m_clockCount >= m_bit_sample_count / 2.0 - 1.0) {
            // Sample in the middle of the bit
            handle_bit_value(m_data);
            m_clockCount -= m_bit_sample_count;
        }

        m_dbg_sample_count++;

        // Periodic stats dump — one line per channel every 30 s
        long long stats_interval = (long long)(DBG_STATS_INTERVAL /
                                               12000.0 * m_sample_rate);
        if (m_dbg_sample_count - m_dbg_stats_sample >= stats_interval) {
            m_dbg_stats_sample = m_dbg_sample_count;
            fprintf(stderr,
                "[DSC-DBG] %-14s  mark_env=%.1f  space_env=%.1f"
                "  clock=%.1f  bits=%lld  sop=%s\n",
                m_dbg_label.c_str(),
                m_mark_env, m_space_env,
                m_clockCount,
                m_dbg_total_bits,
                m_gotSOP ? "YES" : "no");
        }
    }
}

// -----------------------------------------------------------------------
// Envelope / noise decay functions (from navtex_rx)
// -----------------------------------------------------------------------
double dsc_rx::envelope_decay(double avg, double value)
{
    int divisor;
    if (value > avg)
        divisor = static_cast<int>(m_bit_sample_count / 4);
    else
        divisor = static_cast<int>(m_bit_sample_count * 16);
    return decayavg(avg, value, divisor);
}

// -----------------------------------------------------------------------
// handle_bit_value — feeds a demodulated bit into the DSC bit processor
// -----------------------------------------------------------------------
void dsc_rx::handle_bit_value(bool bit)
{
    m_dbg_total_bits++;
    receiveBit(bit);
}

// -----------------------------------------------------------------------
// receiveBit — DSC-specific bit processing
// Adapted from SDRangel DSCDemodSink::receiveBit()
//
// Before phasing: accumulate bits and check every bit against the
// 30-bit phasing pattern table (3 consecutive 10-bit symbols).
// After phasing: every 10 bits, feed into DSCDecoder::decodeBits().
// When the decoder signals message complete, construct DSCMessage
// and invoke the callback.
// -----------------------------------------------------------------------
void dsc_rx::receiveBit(bool bit)
{
    // Store in shift register
    m_bits = (m_bits << 1) | (bit ? 1u : 0u);
    m_bitCount++;

    if (!m_gotSOP) {
        // --- Phasing detection ---
        // Need at least 30 bits (3 × 10-bit symbols) to match
        if (m_bitCount >= 10 * 3) {
            // Check every bit position against phasing patterns
            // (decrement bitCount so we slide by 1 bit each time)
            m_bitCount = 10 * 3 - 1;

            unsigned int pat = m_bits & 0x3FFFFFFFu;  // 30 bits

            // Find best-matching phasing pattern (for near-miss logging)
            int best_dist = 31;
            int best_idx  = -1;
            for (int i = 0; i < DSCDecoder::m_phasingPatternsSize; i++) {
                int d = hamming30(pat, DSCDecoder::m_phasingPatterns[i].m_pattern);
                if (d < best_dist) { best_dist = d; best_idx = i; }
                if (d == 0) break;  // exact match — stop early
            }

            if (best_dist == 0) {
                // Exact phasing pattern match
                m_dscDecoder.init(DSCDecoder::m_phasingPatterns[best_idx].m_offset);
                m_gotSOP   = true;
                m_bitCount = 0;

                // Start RSSI accumulation
                m_rssiMagSqSum   = 0.0;
                m_rssiMagSqCount = 0;

                fprintf(stderr,
                    "[DSC-DBG] %-14s  PHASING LOCK  pat[%d]=0x%08X"
                    "  offset=%d  bits=%lld\n",
                    m_dbg_label.c_str(), best_idx,
                    DSCDecoder::m_phasingPatterns[best_idx].m_pattern,
                    DSCDecoder::m_phasingPatterns[best_idx].m_offset,
                    m_dbg_total_bits);
            } else if (best_dist <= 3) {
                // Near-miss — rate-limited to once per 10 s
                long long nm_interval = (long long)(DBG_NEARMISS_INTERVAL /
                                                    12000.0 * m_sample_rate);
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
        // --- Symbol decoding ---
        // Every 10 bits, extract a symbol and feed to the decoder
        if (m_bitCount == 10) {
            if (m_dscDecoder.decodeBits(m_bits & 0x3FFu)) {
                // Message complete — extract and deliver
                std::vector<unsigned char> bytes = m_dscDecoder.getMessage();
                int errors = m_dscDecoder.getErrors();

                // Compute average RSSI in dB
                float rssi = -100.0f;  // default if no samples
                if (m_rssiMagSqCount > 0) {
                    double avgMagSq = m_rssiMagSqSum / m_rssiMagSqCount;
                    if (avgMagSq > 0.0)
                        rssi = static_cast<float>(10.0 * log10(avgMagSq));
                }

                // Construct DSCMessage and invoke callback
                time_t now = time(nullptr);
                DSCMessage message(bytes, now);

                if (m_callback) {
                    m_callback(message, errors, rssi);
                }

                // Reset for next message
                init();
            }
            m_bitCount = 0;
        }
    }
}

// -----------------------------------------------------------------------
// getStats — return current decoder statistics for web UI
// -----------------------------------------------------------------------
dsc_rx::DecoderStats dsc_rx::getStats() const
{
    DecoderStats s;
    s.signal_level = m_mark_env - m_space_env;  // positive = mark dominant
    s.mark_level   = m_mark_env;
    s.space_level  = m_space_env;
    s.receiving    = m_gotSOP;
    s.bit_count    = m_bitCount;

    if (m_gotSOP && m_rssiMagSqCount > 0) {
        double avgMagSq = m_rssiMagSqSum / m_rssiMagSqCount;
        s.rssi_db = (avgMagSq > 0.0) ? 10.0 * log10(avgMagSq) : -100.0;
    } else {
        s.rssi_db = -100.0;
    }

    return s;
}
