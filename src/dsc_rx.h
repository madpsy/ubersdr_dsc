/* -*- c++ -*- */
/*
 * dsc_rx.h — DSC (Digital Selective Calling) FSK receiver
 *
 * Demodulator ported from SDRangel DSCDemodSink (Jon Beniston, M7RCE):
 *   - Receives CS16 IQ samples at 10000 Hz from ubersdr
 *   - Single complex exponential at ±85 Hz (half the 170 Hz shift)
 *   - FIR lowpass at baud_rate * 1.1 = 110 Hz
 *   - Moving-maximum envelope
 *   - Simplified ATC: bias = abs - 0.5 * env
 *   - Edge-triggered clock recovery with 25% correction
 *
 * DSC framing adapted from SDRangel DSCDemodSink (Jon Beniston, M7RCE)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DSC_RX_H
#define _DSC_RX_H

#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <deque>

#include "dsc_decoder.h"
#include "dsc_message.h"

typedef std::complex<double> cmplx;

// -----------------------------------------------------------------------
// Simple FIR lowpass filter for complex samples
// -----------------------------------------------------------------------
class SimpleLowpass {
public:
    // Create a windowed-sinc FIR lowpass with cutoff fc (normalised 0..0.5)
    // and 'taps' coefficients (must be odd).
    void create(int taps, double fc);
    cmplx filter(cmplx in);

private:
    std::vector<double> m_coeffs;
    std::deque<cmplx>   m_buf;
};

// -----------------------------------------------------------------------
// Moving maximum over a sliding window
// -----------------------------------------------------------------------
class MovingMaximum {
public:
    explicit MovingMaximum(int size = 1) : m_size(size) {}
    void setSize(int size) { m_size = size; m_buf.clear(); }
    void push(double v);
    double getMaximum() const;

private:
    int                m_size;
    std::deque<double> m_buf;
};

// -----------------------------------------------------------------------
// DSC receiver — IQ input, SDRangel-style demodulation
// -----------------------------------------------------------------------
class dsc_rx {
public:
    using MessageCallback = std::function<void(const DSCMessage& msg, int errors, float rssi)>;

    // sample_rate: IQ sample rate from ubersdr (expected 10000 Hz)
    // label: short string for debug log lines, e.g. "16.806 MHz"
    dsc_rx(int sample_rate, MessageCallback callback,
           const std::string &label = "");
    ~dsc_rx() = default;

    // Feed CS16 interleaved I/Q samples (little-endian int16 pairs)
    void process(const int16_t * iq_data, int nb_samples);

    // Statistics for web UI / monitoring
    struct DecoderStats {
        double signal_level;     // mark envelope level
        double mark_level;       // mark channel envelope
        double space_level;      // space channel envelope
        bool   receiving;        // true if phasing detected, message in progress
        int    bit_count;        // bits received in current message
        double rssi_db;          // current RSSI in dB (only valid while receiving)
    };
    DecoderStats getStats() const;

private:
    // ---- Configuration ----
    int    m_sample_rate;
    double m_baud_rate;          // 100.0
    double m_bit_sample_count;   // sample_rate / baud_rate

    // ---- IQ demodulator state (SDRangel style) ----

    // Pre-computed complex exponential table for the half-shift frequency
    // (85 Hz = 170/2).  Length = sample_rate / gcd(sample_rate, 85).
    std::vector<cmplx> m_exp;
    int                m_expIdx;

    // FIR lowpass filters for mark and space channels
    SimpleLowpass m_lpfMark;
    SimpleLowpass m_lpfSpace;

    // Moving-maximum envelope trackers (window = 8 bit periods)
    MovingMaximum m_movMaxMark;
    MovingMaximum m_movMaxSpace;

    // ---- Edge-triggered clock recovery ----
    double m_clockCount;
    bool   m_data;       // current demodulated bit
    bool   m_dataPrev;   // previous bit (for edge detection)

    // ---- Envelope levels (for stats) ----
    double m_markEnv;
    double m_spaceEnv;

    // ---- DSC-specific state ----
    DSCDecoder   m_dscDecoder;
    unsigned int m_bits;
    int          m_bitCount;
    bool         m_gotSOP;

    // RSSI tracking during message reception
    double m_rssiMagSqSum;
    int    m_rssiMagSqCount;

    // Callback for decoded messages
    MessageCallback m_callback;

    // ---- Debug / diagnostics ----
    std::string m_dbg_label;
    long long   m_dbg_sample_count;
    long long   m_dbg_total_bits;
    long long   m_dbg_near_miss_sample;
    long long   m_dbg_stats_sample;

    // ---- Private methods ----
    void processOneSample(cmplx ci);
    void receiveBit(bool bit);
    void init();

    static int hamming30(unsigned int a, unsigned int b);
};

#endif /* _DSC_RX_H */
