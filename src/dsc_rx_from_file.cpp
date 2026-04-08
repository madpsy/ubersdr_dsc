/* -*- c++ -*- */
/*
 * Copyright 2024 Contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Decode DSC messages from a raw PCM audio file (16-bit signed LE mono).
 * Modelled after navtex_rx_from_file.cpp from ubersdr_navtex.
 *
 * Usage:
 *   dsc_rx_from_file [options] <filename>
 *   dsc_rx_from_file [options] -          (read from stdin)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#include "dsc_rx.h"
#include "dsc_message.h"
#include "mmsi.h"
#include "coast_stations.h"

/* ------------------------------------------------------------------ */
/* Usage / help                                                        */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <filename>\n"
        "\n"
        "Decode DSC messages from raw PCM audio file.\n"
        "\n"
        "Options:\n"
        "  --sample-rate <hz>   Input sample rate (default: 12000)\n"
        "  --frequency <hz>     Frequency label for display (default: 0)\n"
        "  --json               Output as JSON (one message per line)\n"
        "  --verbose            Print decoder progress info\n"
        "  --help               Show this help\n"
        "\n"
        "Input format: 16-bit signed little-endian mono PCM\n"
        "Use '-' as filename to read from stdin.\n",
        prog);
}

/* ------------------------------------------------------------------ */
/* Human-readable message printer                                      */
/* ------------------------------------------------------------------ */
static void print_message_human(const DSCMessage &msg, int errors, float rssi,
                                int64_t frequency_hz)
{
    printf("=== DSC Message ===\n");
    printf("  Format:      %s\n", msg.formatSpecifier().c_str());
    if (msg.m_hasCategory)
        printf("  Category:    %s\n", msg.category().c_str());
    printf("  Self ID:     %s", msg.m_selfId.c_str());
    std::string country = MMSI::getCountry(msg.m_selfId);
    std::string mmsi_cat = MMSI::getCategory(msg.m_selfId);
    if (!country.empty())
        printf("  (%s", country.c_str());
    if (!mmsi_cat.empty())
        printf(", %s", mmsi_cat.c_str());
    if (!country.empty() || !mmsi_cat.empty())
        printf(")");
    printf("\n");

    if (msg.m_hasAddress) {
        printf("  Address:     %s", msg.m_address.c_str());
        std::string ac = MMSI::getCountry(msg.m_address);
        if (!ac.empty())
            printf("  (%s)", ac.c_str());
        auto it = CoastStations.find(msg.m_address);
        if (it != CoastStations.end())
            printf("  [%s]", it->second.c_str());
        printf("\n");
    }

    if (msg.m_hasTelecommand1)
        printf("  Telecommand1: %s\n",
               DSCMessage::telecommand1(msg.m_telecommand1).c_str());
    if (msg.m_hasTelecommand2)
        printf("  Telecommand2: %s\n",
               DSCMessage::telecommand2(msg.m_telecommand2).c_str());

    if (msg.m_hasDistressId) {
        printf("  Distress ID: %s", msg.m_distressId.c_str());
        std::string dc = MMSI::getCountry(msg.m_distressId);
        if (!dc.empty())
            printf("  (%s)", dc.c_str());
        printf("\n");
    }
    if (msg.m_hasDistressNature)
        printf("  Distress:    %s\n",
               DSCMessage::distressNature(msg.m_distressNature).c_str());
    if (msg.m_hasPosition)
        printf("  Position:    %s\n", msg.m_position.c_str());
    if (msg.m_hasTime)
        printf("  Time:        %s\n", msg.m_time.c_str());

    printf("  EOS:         %s\n",
           DSCMessage::endOfSignal(msg.m_eos).c_str());
    printf("  ECC:         %s\n", msg.m_eccOk ? "OK" : "ERROR");
    printf("  Valid:       %s\n", msg.m_valid ? "yes" : "no");
    printf("  Errors:      %d\n", errors);
    printf("  RSSI:        %.1f dB\n", rssi);
    if (frequency_hz > 0)
        printf("  Frequency:   %lld Hz\n", (long long)frequency_hz);
    printf("  Received:    %s\n", msg.m_receivedAt.c_str());
    printf("===================\n\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, const char **argv)
{
    int sample_rate = 12000;
    int64_t frequency_hz = 0;
    bool json_mode = false;
    bool verbose = false;
    const char *filename = nullptr;

    /* ---- Parse arguments ---- */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--sample-rate") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing value for --sample-rate\n"); return 1; }
            sample_rate = atoi(argv[i]);
            if (sample_rate <= 0) { fprintf(stderr, "Invalid sample rate: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--frequency") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing value for --frequency\n"); return 1; }
            frequency_hz = atoll(argv[i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            json_mode = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            if (filename) {
                fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
            filename = argv[i];
        }
    }

    if (!filename) {
        print_usage(argv[0]);
        return 1;
    }

    /* ---- Open input ---- */
    int fd;
    if (strcmp(filename, "-") == 0) {
        fd = fileno(stdin);
    } else {
        int flags = O_RDONLY;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        fd = open(filename, flags);
        if (fd == -1) {
            fprintf(stderr, "open(%s) failed: %s\n", filename, strerror(errno));
            return 1;
        }
    }

    /* Disable buffering on stdout for immediate output */
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (verbose) {
        fprintf(stderr, "[dsc_rx_from_file] sample_rate=%d frequency=%lld file=%s json=%d\n",
                sample_rate, (long long)frequency_hz, filename, json_mode);
    }

    /* ---- Create decoder ---- */
    int message_count = 0;

    dsc_rx rx(sample_rate,
        [&](const DSCMessage &msg, int errors, float rssi) {
            message_count++;

            /* Stamp the frequency onto the message (const_cast needed since
               the callback gives us a const ref, but m_frequencyHz is a
               display-only field set externally) */
            const_cast<DSCMessage &>(msg).m_frequencyHz = frequency_hz;

            if (json_mode) {
                printf("%s\n", msg.toJson().c_str());
                fflush(stdout);
            } else {
                print_message_human(msg, errors, rssi, frequency_hz);
            }

            if (verbose) {
                fprintf(stderr, "[dsc_rx_from_file] message #%d  errors=%d rssi=%.1f ecc=%s valid=%s\n",
                        message_count, errors, rssi,
                        msg.m_eccOk ? "ok" : "ERR",
                        msg.m_valid ? "yes" : "no");
            }
        });

    /* ---- Read loop ---- */
    constexpr int BUFSIZE = 1024;
    int16_t inbuf[BUFSIZE];
    long long total_samples = 0;

    while (true) {
        ssize_t nread = read(fd, inbuf, BUFSIZE * sizeof(int16_t));
        if (nread < 0) {
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
            if (fd != fileno(stdin))
                close(fd);
            return 1;
        }
        if (nread == 0)
            break;

        int nb_int16 = nread / sizeof(int16_t);
        total_samples += nb_int16;

        /* process() expects CS16 interleaved I/Q pairs */
        rx.process(inbuf, nb_int16 / 2);

        if (verbose && (total_samples % (sample_rate * 10) < BUFSIZE)) {
            double seconds = (double)total_samples / sample_rate;
            dsc_rx::DecoderStats stats = rx.getStats();
            fprintf(stderr, "[dsc_rx_from_file] %.1fs  samples=%lld  messages=%d  "
                    "signal=%.3f  receiving=%s\n",
                    seconds, total_samples, message_count,
                    stats.signal_level, stats.receiving ? "yes" : "no");
        }
    }

    if (verbose) {
        fprintf(stderr, "[dsc_rx_from_file] EOF — %lld samples (%.1fs), %d messages decoded\n",
                total_samples, (double)total_samples / sample_rate, message_count);
    }

    if (fd != fileno(stdin))
        close(fd);

    return 0;
}
