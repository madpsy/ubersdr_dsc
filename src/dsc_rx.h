/* -*- c++ -*- */
/*
 * dsc_rx.h — DSC (Digital Selective Calling) FSK receiver
 *
 * FSK demodulator adapted from navtex_rx (Franco Venturi / Rik van Riel)
 * DSC framing adapted from SDRangel dscdemodsink (Jon Beniston, M7RCE)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DSC_RX_H
#define _DSC_RX_H

#include <complex>
#include <cstdint>
#include <functional>
#include <string>

#include "dsc_decoder.h"
#include "dsc_message.h"

class fftfilt;
typedef std::complex<double> cmplx;

class dsc_rx {
public:
    using MessageCallback = std::function<void(const DSCMessage& msg, int errors, float rssi)>;

    // label is a short string identifying this channel (e.g. "16.806 MHz")
    // used as a prefix in debug log lines so 25 channels can be told apart
    dsc_rx(int sample_rate, MessageCallback callback,
           const std::string &label = "");
    ~dsc_rx();

    // Feed audio samples — main entry point
    void process(const float * data, int nb_samples);
    void process(const int16_t * data, int nb_samples);

    // Statistics for web UI / monitoring
    struct DecoderStats {
        double signal_level;     // prompt accumulator average magnitude
        double mark_level;       // mark envelope level
        double space_level;      // space envelope level
        bool   receiving;        // true if phasing detected, message in progress
        int    bit_count;        // bits received in current message
        double rssi_db;          // current RSSI in dB (only valid while receiving)
    };
    DecoderStats getStats() const;

private:
    // ---- FSK demodulator state (from navtex_rx) ----

    int m_sample_rate;

    // Filter parameters
    double m_center_frequency_f;
    double m_baud_rate;

    double m_mark_f;
    double m_space_f;
    double m_mark_phase;
    double m_space_phase;

    fftfilt *m_mark_lowpass;
    fftfilt *m_space_lowpass;

    double m_bit_sample_count;   // samples per bit (fractional)

    // Edge-triggered clock recovery (SDRangel style)
    // m_clockCount counts samples within a bit period.
    // Initialised to -m_bit_sample_count/2 so the first bit fires after
    // one full bit period.  On each 0→1 data transition the count is
    // pulled 25% toward zero to track the signal timing.
    double m_clockCount;
    bool   m_data;       // current demodulated bit (post-ATC)
    bool   m_dataPrev;   // previous demodulated bit (for edge detection)

    // Envelope / ATC state (per-instance, not static)
    double m_mark_env;
    double m_space_env;

    // ---- DSC-specific state ----

    DSCDecoder m_dscDecoder;
    unsigned int m_bits;         // bit shift register (accumulates up to 30 bits)
    int m_bitCount;
    bool m_gotSOP;               // start of phasing detected

    // RSSI tracking during message reception
    double m_rssiMagSqSum;
    int    m_rssiMagSqCount;

    // Callback for decoded messages
    MessageCallback m_callback;

    // ---- Debug / diagnostics ----

    std::string m_dbg_label;          // channel label for log lines
    long long   m_dbg_sample_count;   // total samples seen (for periodic stats)
    long long   m_dbg_total_bits;     // total bits produced (confirms bit flow)
    long long   m_dbg_near_miss_sample; // sample count at last near-miss log
    long long   m_dbg_stats_sample;   // sample count at last stats dump

    // ---- Private methods ----

    // Filter setup
    void set_filter_values();
    void configure_filters();

    // DSP pipeline
    cmplx mixer(double & phase, double f, cmplx in);
    void process_fft_output(cmplx * zp_mark, cmplx * zp_space, int samples);
    double envelope_decay(double avg, double value);

    // Bit-level processing
    void handle_bit_value(bool bit);
    void receiveBit(bool bit);

    // Reset decoder state
    void init();
};

#endif /* _DSC_RX_H */
