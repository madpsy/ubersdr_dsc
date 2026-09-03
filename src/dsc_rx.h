/* -*- c++ -*- */
/*
 * dsc_rx.h — DSC (Digital Selective Calling) FSK receiver
 *
 * Direct port of SDRangel DSCDemodSink (Jon Beniston, M7RCE), scaled from
 * SDRangel's internal 1000 Hz to whatever rate ubersdr delivers.
 *
 * The scale is taken from the rate the receiver is CONSTRUCTED with, not
 * assumed: the server's [iq] preset is 12 kHz, and a demodulator built for
 * 10 kHz would have every tone spacing, filter cutoff and symbol period out by
 * a factor of 1.2 while still producing plausible-looking output.
 *
 * Signal chain (identical to SDRangel, all constants × rateScale = rate/1000):
 *   - Complex exponential table at FREQUENCY_SHIFT/2 = 85 Hz (len 600×scale)
 *   - Two FIR lowpass filters: 301 taps, cutoff = BAUD_RATE * 1.1 = 110 Hz
 *   - Two MovingMaximum envelope trackers: window = samplesPerBit * 8
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

#include "firfilter.h"
#include "dsc_decoder.h"
#include "dsc_message.h"

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
// DSC receiver — IQ input, SDRangel-compatible demodulation at any rate that
// is an integer multiple of 1000 Hz
// -----------------------------------------------------------------------
class dsc_rx {
public:
    using MessageCallback = std::function<void(const DSCMessage& msg, int errors, float rssi)>;

    // Everything the sample rate determines, in one place.
    //
    // Exposed so it can be asserted against: a demodulator tuned to the wrong
    // rate does not fail, it just quietly stops decoding, so the only way to
    // know these tracked the rate is to look at them.
    struct RateConfig {
        bool   supported;       // false: rate not an integer multiple of 1000 Hz
        int    sample_rate;
        int    rate_scale;      // sample_rate / 1000
        int    exp_length;      // 600 * rate_scale
        int    samples_per_bit; // sample_rate / 100
        int    movmax_window;   // samples_per_bit * 8
        double phase_step;      // radians/sample of the 85 Hz exponential
        double lpf_sample_rate; // sample rate the 301-tap lowpass is designed at
        double lpf_cutoff_hz;   // 110 Hz, independent of the sample rate
    };

    // Derive the configuration for a rate without building a receiver.
    static RateConfig rateConfigFor(int sample_rate);

    // sample_rate: IQ sample rate from ubersdr.  Must be an integer multiple of
    // 1000 Hz; anything else is refused (see rateConfigFor) and the receiver
    // then discards samples rather than decoding them mistuned.
    // label: short string for log lines, e.g. "16.806 MHz"
    dsc_rx(int sample_rate, MessageCallback callback,
           const std::string &label = "");

    // What this instance was actually built with.
    RateConfig getRateConfig() const { return m_rate; }
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
    int        m_sample_rate;
    RateConfig m_rate;          // everything derived from m_sample_rate

    // ---- IQ demodulator state ----
    std::vector<Complex> m_exp;      // complex exponential table
    int                  m_expIdx;

    Lowpass<Complex> m_lpfMark;    // FIR lowpass, mark channel
    Lowpass<Complex> m_lpfSpace;   // FIR lowpass, space channel

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
    void processOneSample(Complex ci);
    void receiveBit(bool bit);
    void init();
};

#endif /* _DSC_RX_H */
