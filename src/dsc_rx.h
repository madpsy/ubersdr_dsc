/* -*- c++ -*- */
/*
 * dsc_rx.h — DSC (Digital Selective Calling) FSK receiver
 *
 * Direct port of SDRangel DSCDemodSink (Jon Beniston, M7RCE) scaled for
 * 10000 Hz input from ubersdr (SDRangel uses 1000 Hz internally).
 *
 * Signal chain (identical to SDRangel, all constants × RATE_SCALE=10):
 *   - Complex exponential table at FREQUENCY_SHIFT/2 = 85 Hz (len 6000)
 *   - Two FIR lowpass filters: 301 taps, cutoff = BAUD_RATE * 1.1 = 110 Hz
 *   - Two MovingMaximum envelope trackers: window = samplesPerBit * 8 = 800
 *   - ATC: bias = abs - 0.5 * env
 *   - Clock recovery: rising edge (0→1) only, 25% correction
 *   - Phasing: exact 30-bit pattern match
 *   - Symbol decode: every 10 bits via DSCDecoder
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
// FIR lowpass filter for complex samples — circular buffer implementation
// (mirrors SDRangel's Lowpass<Complex>)
// -----------------------------------------------------------------------
class SimpleLowpass {
public:
    void create(int taps, double fc);  // fc normalised 0..0.5
    cmplx filter(cmplx in);

private:
    std::vector<double> m_coeffs;
    std::vector<cmplx>  m_buf;   // circular buffer
    int                 m_pos;   // write position
};

// -----------------------------------------------------------------------
// Moving maximum over a sliding window (mirrors SDRangel's MovingMaximum)
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
// DSC receiver — IQ input, SDRangel-compatible demodulation at 10000 Hz
// -----------------------------------------------------------------------
class dsc_rx {
public:
    using MessageCallback = std::function<void(const DSCMessage& msg, int errors, float rssi)>;

    // sample_rate: IQ sample rate from ubersdr (must be 10000 Hz)
    // label: short string for log lines, e.g. "16.806 MHz"
    dsc_rx(int sample_rate, MessageCallback callback,
           const std::string &label = "");
    ~dsc_rx() = default;

    // Feed CS16 interleaved I/Q samples (little-endian int16 pairs)
    void process(const int16_t *iq_data, int nb_samples);

    // Statistics for web UI / monitoring
    struct DecoderStats {
        double signal_level;  // mark_env - space_env
        double mark_level;    // mark channel envelope
        double space_level;   // space channel envelope
        bool   receiving;     // true if phasing locked, message in progress
        int    bit_count;     // bits received in current message
        double rssi_db;       // RSSI in dB (valid while receiving)
    };
    DecoderStats getStats() const;

private:
    // ---- Configuration ----
    int m_sample_rate;

    // ---- IQ demodulator state ----
    std::vector<cmplx> m_exp;      // complex exponential table (len 6000)
    int                m_expIdx;

    SimpleLowpass m_lpfMark;       // FIR lowpass, mark channel
    SimpleLowpass m_lpfSpace;      // FIR lowpass, space channel

    MovingMaximum m_movMaxMark;    // envelope tracker, mark
    MovingMaximum m_movMaxSpace;   // envelope tracker, space

    // ---- Clock recovery ----
    double m_clockCount;
    bool   m_data;      // current bit
    bool   m_dataPrev;  // previous bit (for rising-edge detection)

    // ---- Envelope levels (for stats) ----
    double m_markEnv;
    double m_spaceEnv;

    // ---- DSC framing state ----
    DSCDecoder   m_dscDecoder;
    unsigned int m_bits;
    int          m_bitCount;
    bool         m_gotSOP;

    // ---- RSSI accumulation ----
    double m_rssiMagSqSum;
    int    m_rssiMagSqCount;

    // ---- Callback ----
    MessageCallback m_callback;

    // ---- Debug ----
    std::string m_dbg_label;
    long long   m_dbg_sample_count;

    // ---- Private methods ----
    void processOneSample(cmplx ci);
    void receiveBit(bool bit);
    void init();
};

#endif /* _DSC_RX_H */
