/* dsc_rate_test.cpp — that the demodulator is tuned to the rate it is given.
 *
 * WHY THIS EXISTS
 * ---------------
 * dsc_rx derives its tone spacing, filter cutoffs, symbol period and envelope
 * window from the sample rate.  It used to derive them from a hardcoded scale of
 * 10 instead, so a 12 kHz stream -- which is what the server's [iq] preset
 * delivers -- was demodulated as if it were 10 kHz: every one of those out by a
 * factor of 1.2.
 *
 * That failure is silent in the worst way.  A mistuned demodulator and a
 * correctly tuned one both produce nothing on a quiet channel, so "it ran
 * without erroring" proves nothing at all, and DSC traffic is far too
 * intermittent to wait for.  So this does two things:
 *
 *   1. Asserts the derived constants track the rate, and that the 10 kHz case
 *      still reproduces the values the code was originally written with, bit
 *      for bit -- so the fix cannot have quietly changed behaviour for the rate
 *      that used to work.
 *
 *   2. Modulates a real DSC symbol stream and asserts the decoder recovers the
 *      exact symbols, at both rates.  The same waveform is then fed to a
 *      receiver built for the wrong rate, which must NOT decode it: that is the
 *      bug reproduced on demand, and the proof that case 1 is load-bearing
 *      rather than decorative.
 *
 * Build:  cmake --build build --target dsc_rate_test && ./build/test/dsc_rate_test
 */

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "../src/dsc_rx.h"
#include "../src/dsc_decoder.h"

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
// DSC symbol encoder — the inverse of DSCDecoder::bitsToSymbol.
//
// bitsToSymbol computes `reverse(bits >> 3) >> 1`, so the top 7 bits of the
// codeword are the bit-reversal of the symbol, and the low 3 are the number of
// zeros in it.  Checked against the phasing table below, which is the same
// encoding written out by hand.
// ---------------------------------------------------------------------------
static unsigned int symbolToBits(int s)
{
    unsigned int rev = 0;
    int ones = 0;
    for (int i = 0; i < 7; i++) {
        if (s & (1 << i)) { rev |= 1u << (6 - i); ones++; }
    }
    return (rev << 3) | (unsigned int)(7 - ones);
}

// ---------------------------------------------------------------------------
// Modulator: symbols -> 10-bit codewords -> continuous-phase FSK IQ.
//
// dsc_rx correlates against exp(+j*2*pi*85*t) for the mark channel, so a mark
// (a 1 bit) is a tone 85 Hz BELOW the tuned centre and a space is 85 Hz above.
// That is the convention of the SDRangel demodulator this is a port of, not an
// independent claim about what is on air.
// ---------------------------------------------------------------------------
static void modulateBit(std::vector<int16_t> &iq, bool bit, int samples_per_bit,
                        int sample_rate, double &phase, double amplitude)
{
    const double shift_hz = bit ? -85.0 : +85.0;
    const double step = 2.0 * M_PI * shift_hz / (double)sample_rate;
    for (int i = 0; i < samples_per_bit; i++) {
        iq.push_back((int16_t)(amplitude * std::cos(phase)));
        iq.push_back((int16_t)(amplitude * std::sin(phase)));
        phase += step;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        if (phase < 0.0) phase += 2.0 * M_PI;
    }
}

static std::vector<int16_t> modulate(const std::vector<int> &symbols,
                                     int dot_bits, int sample_rate)
{
    const int samples_per_bit = sample_rate / 100;   // 100 baud
    std::vector<int16_t> iq;
    double phase = 0.0;

    // Dot pattern: alternating bits, so clock recovery locks and both envelope
    // trackers fill before anything that has to decode correctly arrives.
    for (int i = 0; i < dot_bits; i++)
        modulateBit(iq, (i % 2) == 0, samples_per_bit, sample_rate, phase, 8000.0);

    for (int s : symbols) {
        const unsigned int bits = symbolToBits(s);
        for (int b = 9; b >= 0; b--)   // MSB first, as dsc_rx shifts them in
            modulateBit(iq, (bits >> b) & 1u, samples_per_bit, sample_rate, phase, 8000.0);
    }
    return iq;
}

// ---------------------------------------------------------------------------
// A DSC transmission, in the order dsc_rx's framing expects to see it.
//
// The receiver locks on an exact 30-bit match of three phasing codewords, then
// consumes nine more phasing symbols, then a DX/RX time-diversity stream in
// which each message symbol is repeated three symbol positions later.
// ---------------------------------------------------------------------------
static std::vector<int> buildTransmission(const std::vector<int> &dx)
{
    std::vector<int> tx;

    // The 30-bit pattern the detector matches (125 111 125), then the phasing
    // symbols it expects to follow it.
    const int phasing[] = {125, 111, 125,
                           110, 125, 109, 125, 108, 125, 107, 125, 106};
    for (int s : phasing) tx.push_back(s);

    // Fill: DX, RX(106), DX, RX(105), DX.
    tx.push_back(dx[0]);
    tx.push_back(106);
    tx.push_back(dx[1]);
    tx.push_back(105);
    tx.push_back(dx[2]);

    // Then alternating RX(dx[k]) and DX(dx[k+3]).
    for (size_t k = 0; k + 3 < dx.size(); k++) {
        tx.push_back(dx[k]);
        tx.push_back(dx[k + 3]);
    }
    return tx;
}

// ---------------------------------------------------------------------------
// The constants, and that they still match the originals at 10 kHz
// ---------------------------------------------------------------------------
static void test_rate_config()
{
    printf("rate-derived constants\n");

    dsc_rx r10(10000, nullptr, "10k");
    dsc_rx r12(12000, nullptr, "12k");
    const dsc_rx::RateConfig a = r10.getRateConfig();
    const dsc_rx::RateConfig b = r12.getRateConfig();

    check(a.supported && b.supported, "both rates are supported");

    // The values the file was written with, spelled out rather than recomputed:
    // RATE_SCALE 10, EXP_LENGTH 600*10, SAMPLES_PER_BIT 1000*10/100, window *8,
    // step 2*pi*85/10000, lowpass designed at 10000 Hz with a 110 Hz cutoff.
    check(a.rate_scale      == 10,   "10 kHz: rate scale is still 10");
    check(a.exp_length      == 6000, "10 kHz: exponential table is still 6000");
    check(a.samples_per_bit == 100,  "10 kHz: samples per bit is still 100");
    check(a.movmax_window   == 800,  "10 kHz: envelope window is still 800");
    check(std::fabs(a.phase_step - 2.0 * M_PI * 85.0 / 10000.0) < 1e-15,
          "10 kHz: phase step is bit-for-bit the original");
    check(a.lpf_sample_rate == 10000.0, "10 kHz: lowpass is designed at 10000 Hz");
    // Compared against the original expression rather than the literal 110.0:
    // 100 * 1.1 is 110.00000000000001 in binary floating point, and it is that
    // value, not a rounded one, that the filter was always designed with.
    check(a.lpf_cutoff_hz   == 100 * 1.1,
          "10 kHz: lowpass cutoff is bit-for-bit the original 100 * 1.1");

    // At 12 kHz every sample-count constant scales by exactly 1.2 and the phase
    // step shrinks by the same ratio -- 85 Hz is 85 Hz either way.
    check(b.rate_scale      == 12,   "12 kHz: rate scale is 12");
    check(b.exp_length      == 7200, "12 kHz: exponential table is 600 x 12");
    check(b.samples_per_bit == 120,  "12 kHz: samples per bit is 120");
    check(b.movmax_window   == 960,  "12 kHz: envelope window is 120 x 8");
    check(std::fabs((double)b.exp_length / a.exp_length - 1.2) < 1e-12,
          "the exponential table grew by exactly the rate ratio");
    check(std::fabs((double)b.samples_per_bit / a.samples_per_bit - 1.2) < 1e-12,
          "the symbol period grew by exactly the rate ratio");
    check(std::fabs(a.phase_step / b.phase_step - 1.2) < 1e-12,
          "the phase step shrank by exactly the rate ratio");
    check(b.lpf_sample_rate == 12000.0,
          "the lowpass is designed at 12000 Hz, not 10000");
    check(b.lpf_cutoff_hz == a.lpf_cutoff_hz,
          "the cutoff stays 110 Hz in absolute terms at both rates");

    // The exponential table must hold a whole number of 85 Hz cycles, or the
    // oscillator steps in phase every time the index wraps.
    check(std::fabs(a.exp_length * a.phase_step - 2.0 * M_PI * 51.0) < 1e-9,
          "10 kHz: the table closes on exactly 51 cycles");
    check(std::fabs(b.exp_length * b.phase_step - 2.0 * M_PI * 51.0) < 1e-9,
          "12 kHz: the table closes on exactly 51 cycles too");

    // A rate that does not divide is refused, not rounded.
    check(!dsc_rx::rateConfigFor(11025).supported, "11025 Hz is refused");
    check(!dsc_rx::rateConfigFor(0).supported,     "a zero rate is refused");
    check(dsc_rx::rateConfigFor(48000).supported,  "48 kHz would be accepted");
}

// ---------------------------------------------------------------------------
// That it actually demodulates, at both rates
// ---------------------------------------------------------------------------
struct Capture {
    bool                       got = false;
    std::vector<unsigned char> bytes;
    int                        errors = -1;
};

static bool decodeAt(int gen_rate, int rx_rate, Capture &cap,
                     const std::vector<int> &dx)
{
    const std::vector<int>     tx = buildTransmission(dx);
    const std::vector<int16_t> iq = modulate(tx, 240, gen_rate);

    dsc_rx rx(rx_rate, [&cap](const DSCMessage &m, int errors, float) {
        if (cap.got) return;
        cap.got    = true;
        cap.bytes  = m.m_data;
        cap.errors = errors;
    }, "test");

    // In 4096-pair chunks, the way the WebSocket path feeds it.
    const int total = (int)(iq.size() / 2);
    for (int off = 0; off < total; off += 4096) {
        const int n = (off + 4096 <= total) ? 4096 : (total - off);
        rx.process(&iq[(size_t)off * 2], n);
    }
    return cap.got;
}

static void test_demodulates()
{
    printf("synthetic DSC burst\n");

    // Symbols chosen to be distinct and unambiguous; 127 is the end-of-sequence
    // that terminates the message.  Not a semantically meaningful call -- this
    // is a symbol-level test of the demodulator, not of message parsing.
    const std::vector<int> dx = {112, 70, 71, 72, 73, 74, 75, 127, 76, 77, 78, 79};
    const std::vector<unsigned char> want = {112, 70, 71, 72, 73, 74, 75, 127};

    Capture c12;
    check(decodeAt(12000, 12000, c12, dx),
          "12 kHz: a 12 kHz burst decodes -- the rate the server actually sends");
    if (c12.got) {
        bool match = c12.bytes.size() >= want.size();
        for (size_t i = 0; match && i < want.size(); i++)
            match = (c12.bytes[i] == want[i]);
        check(match, "12 kHz: every symbol comes back exactly as transmitted");
        check(c12.errors == 0, "12 kHz: with no error corrections used");
    }

    Capture c10;
    check(decodeAt(10000, 10000, c10, dx),
          "10 kHz: the rate the code was written for still decodes");
    if (c10.got) {
        bool match = c10.bytes.size() >= want.size();
        for (size_t i = 0; match && i < want.size(); i++)
            match = (c10.bytes[i] == want[i]);
        check(match, "10 kHz: every symbol comes back exactly as transmitted");
        check(c10.errors == 0, "10 kHz: with no error corrections used");
    }

    // The bug, on demand: a 12 kHz waveform into a receiver built for 10 kHz is
    // exactly what the old file-scope constants did to every channel.  It must
    // not decode -- if it did, the constants above would not be load-bearing.
    Capture bug;
    decodeAt(12000, 10000, bug, dx);
    check(!bug.got,
          "a 12 kHz burst into a 10 kHz receiver does NOT decode (the old bug)");

    // And the converse, so the failure above is about the mismatch rather than
    // about 10 kHz receivers being broken in general.
    Capture bug2;
    decodeAt(10000, 12000, bug2, dx);
    check(!bug2.got,
          "nor does a 10 kHz burst into a 12 kHz receiver");
}

static void test_unsupported_rate_is_inert()
{
    printf("unsupported rate\n");

    const std::vector<int> dx = {112, 70, 71, 72, 73, 74, 75, 127, 76, 77, 78, 79};
    Capture cap;
    decodeAt(11025, 11025, cap, dx);
    check(!cap.got, "a rate that cannot be handled decodes nothing at all");
    check(!dsc_rx(11025, nullptr, "odd").getRateConfig().supported,
          "and says so rather than pretending");
}

int main()
{
    printf("dsc_rx sample-rate handling\n\n");
    test_rate_config();
    test_demodulates();
    test_unsupported_rate_is_inert();
    printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
