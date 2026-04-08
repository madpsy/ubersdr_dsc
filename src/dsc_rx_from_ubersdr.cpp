/* -*- c++ -*- */
/*
 * Copyright 2024 Contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Multi-frequency DSC decoder for ka9q_ubersdr servers.
 *
 * Usage:
 *   dsc_rx_from_ubersdr [--sdr-url URL] [--web-port N] [--freqs f1,f2,...]
 *
 * Each frequency gets its own ubersdr WebSocket connection and DSC decoder
 * via the ChannelManager class.  Decoded messages are broadcast to browser
 * clients via a built-in web UI with three tabs:
 *   1. Messages -- live decoded DSC message table
 *   2. Frequency Monitor -- per-channel status and metrics
 *   3. Audio Preview -- listen to raw audio from any channel
 */

#include "channel_manager.h"
#include "dsc_rx.h"
#include "dsc_message.h"
#include "mmsi.h"
#include "coast_stations.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXHttpServer.h>

/* ------------------------------------------------------------------ */
/* DSC Frequencies (from ITU-R M.541 / SDRangel)                       */
/* ------------------------------------------------------------------ */
static std::vector<int64_t> get_default_dsc_frequencies()
{
    std::vector<int64_t> freqs = {
        2177000, 2189500,
        4208000, 4208500, 4209000,
        6312500, 6313000,
        8415000, 8415500, 8416000,
        12577500, 12578000, 12578500,
        16805000, 16805500, 16806000,
        18898500, 18899000, 18899500,
        22374500, 22375000, 22375500,
        25208500, 25209000, 25209500,
    };
    std::sort(freqs.begin(), freqs.end());
    freqs.erase(std::unique(freqs.begin(), freqs.end()), freqs.end());
    return freqs;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static std::string freq_band_name(int64_t f)
{
    double m = f / 1e6;
    if (m < 3)  return "MF";
    if (m < 5)  return "4 MHz";
    if (m < 7)  return "6 MHz";
    if (m < 10) return "8 MHz";
    if (m < 14) return "12 MHz";
    if (m < 18) return "16 MHz";
    if (m < 20) return "18 MHz";
    if (m < 24) return "22 MHz";
    if (m < 30) return "25 MHz";
    return "HF";
}

static std::string freq_mhz(int64_t f)
{
    char b[32]; snprintf(b, sizeof(b), "%.4f", f / 1e6); return b;
}

static std::string json_escape(const std::string &s)
{
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char h[8]; snprintf(h, sizeof(h), "\\u%04x", (unsigned char)c); o += h;
            } else o += c;
        }
    }
    return o;
}

static std::string strip_trailing_slash(const std::string &s)
{
    std::string r = s;
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r;
}

/* ------------------------------------------------------------------ */
/* BroadcastHub                                                         */
/* ------------------------------------------------------------------ */
struct BroadcastHub {
    ix::HttpServer *server = nullptr;
    std::mutex audio_mu;
    std::map<ix::WebSocket*, int64_t> audio_subs;

    void sub_audio(ix::WebSocket *ws, int64_t f) {
        std::lock_guard<std::mutex> l(audio_mu); audio_subs[ws] = f;
    }
    void unsub_audio(ix::WebSocket *ws) {
        std::lock_guard<std::mutex> l(audio_mu); audio_subs.erase(ws);
    }

    void send_audio(int64_t freq, const std::vector<int16_t> &pcm) {
        if (!server || pcm.empty()) return;
        std::lock_guard<std::mutex> l(audio_mu);
        if (audio_subs.empty()) return;
        std::string frame(1 + 8 + pcm.size() * 2, '\0');
        frame[0] = 0x04;
        uint64_t fv = (uint64_t)freq;
        memcpy(&frame[1], &fv, 8);
        memcpy(&frame[9], pcm.data(), pcm.size() * 2);
        auto clients = server->getClients();
        for (auto &ws : clients) {
            auto it = audio_subs.find(ws.get());
            if (it != audio_subs.end() && it->second == freq)
                ws->sendBinary(frame);
        }
    }

    void send_json(const std::string &j) {
        if (!server) return;
        for (auto &ws : server->getClients()) ws->sendText(j);
    }

    void send_message(const DecodedMessage &dm) {
        if (!server) return;
        std::string sc = MMSI::getCountry(dm.message.m_selfId);
        std::string scat = MMSI::getCategory(dm.message.m_selfId);
        std::string ac, cs;
        if (dm.message.m_hasAddress) {
            ac = MMSI::getCountry(dm.message.m_address);
            auto it = CoastStations.find(dm.message.m_address);
            if (it != CoastStations.end()) cs = it->second;
        }
        std::string dc, dcs;
        if (dm.message.m_hasDistressId) {
            dc = MMSI::getCountry(dm.message.m_distressId);
            auto it = CoastStations.find(dm.message.m_distressId);
            if (it != CoastStations.end()) dcs = it->second;
        }
        std::string j = "{\"type\":\"message\",\"data\":";
        j += dm.message.toJson();
        if (!j.empty() && j.back() == '}') j.pop_back();
        j += ",\"selfCountry\":\"" + json_escape(sc) + "\"";
        j += ",\"selfCategory\":\"" + json_escape(scat) + "\"";
        if (dm.message.m_hasAddress) {
            j += ",\"addressCountry\":\"" + json_escape(ac) + "\"";
            if (!cs.empty()) j += ",\"coastStation\":\"" + json_escape(cs) + "\"";
        }
        if (dm.message.m_hasDistressId) {
            j += ",\"distressCountry\":\"" + json_escape(dc) + "\"";
            if (!dcs.empty()) j += ",\"distressCoastStation\":\"" + json_escape(dcs) + "\"";
        }
        char buf[128];
        snprintf(buf, sizeof(buf), ",\"rxErrors\":%d,\"rxRssi\":%.1f,\"rxFrequencyHz\":%lld",
                 dm.errors, dm.rssi, (long long)dm.frequency_hz);
        j += buf;
        j += ",\"rxFrequencyMHz\":\"" + freq_mhz(dm.frequency_hz) + "\"";
        j += ",\"rxBand\":\"" + json_escape(freq_band_name(dm.frequency_hz)) + "\"";
        j += "}}";
        send_json(j);
    }

    void send_metrics(const std::vector<ChannelMetrics> &metrics) {
        if (!server) return;
        std::string j = "{\"type\":\"metrics\",\"data\":[";
        for (size_t i = 0; i < metrics.size(); i++) {
            const auto &m = metrics[i];
            if (i) j += ",";
            double ar = m.rssi_count > 0 ? m.rssi_sum / m.rssi_count : 0.0;
            double er = m.message_count > 0 ? (double)m.total_errors / m.message_count * 100.0 : 0.0;
            char b[512];
            snprintf(b, sizeof(b),
                "{\"frequencyHz\":%lld,\"frequencyMHz\":\"%s\",\"band\":\"%s\""
                ",\"connected\":%s,\"enabled\":%s,\"messageCount\":%d"
                ",\"validMessageCount\":%d,\"totalErrors\":%d,\"errorRate\":%.1f"
                ",\"avgRssi\":%.1f,\"lastMessageTime\":%lld,\"reconnectCount\":%d}",
                (long long)m.frequency_hz, freq_mhz(m.frequency_hz).c_str(),
                freq_band_name(m.frequency_hz).c_str(),
                m.connected ? "true" : "false", m.enabled ? "true" : "false",
                m.message_count, m.valid_message_count, m.total_errors, er, ar,
                (long long)m.last_message_time, m.reconnect_count);
            j += b;
        }
        j += "]}";
        send_json(j);
    }
};

static BroadcastHub *g_hub = nullptr;
static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

/* ------------------------------------------------------------------ */
/* Embedded HTML page (in separate header for size management)          */
/* ------------------------------------------------------------------ */
#include "dsc_web_ui.h"

/* ------------------------------------------------------------------ */
/* Parse comma-separated frequency list                                 */
/* ------------------------------------------------------------------ */
static std::vector<int64_t> parse_freq_list(const std::string &s)
{
    std::vector<int64_t> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        /* Trim whitespace */
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        token = token.substr(start, end - start + 1);
        char *endp = nullptr;
        int64_t f = strtoll(token.c_str(), &endp, 10);
        if (endp && *endp == '\0' && f > 0)
            result.push_back(f);
    }
    /* Deduplicate */
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, const char **argv)
{
    std::string sdr_url = "http://localhost:8073";
    int web_port = 6093;
    std::vector<int64_t> frequencies;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--sdr-url") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --sdr-url requires a value\n");
                return EXIT_FAILURE;
            }
            sdr_url = argv[++i];
        } else if (arg == "--web-port") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --web-port requires a value\n");
                return EXIT_FAILURE;
            }
            char *end = nullptr;
            web_port = (int)strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || web_port <= 0 || web_port > 65535) {
                fprintf(stderr, "invalid web port: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else if (arg == "--freqs") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --freqs requires a value\n");
                return EXIT_FAILURE;
            }
            frequencies = parse_freq_list(argv[++i]);
            if (frequencies.empty()) {
                fprintf(stderr, "error: no valid frequencies in --freqs\n");
                return EXIT_FAILURE;
            }
        } else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "Usage: %s [--sdr-url URL] [--web-port N] [--freqs f1,f2,...]\n"
                "\n"
                "  --sdr-url URL    ubersdr server base URL (default: http://localhost:8073)\n"
                "  --web-port N     local web UI port (default: 6093)\n"
                "  --freqs f1,f2    comma-separated frequencies in Hz\n"
                "                   (default: all 25 ITU DSC frequencies)\n"
                "\n"
                "Examples:\n"
                "  %s --sdr-url http://192.168.1.10:8073\n"
                "  %s --freqs 2187500,8414500,16804500\n"
                "  %s --sdr-url http://sdr:8073 --web-port 8080 --freqs 2187500\n",
                argv[0], argv[0], argv[0], argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return EXIT_FAILURE;
        }
    }

    /* Strip trailing slash from SDR URL */
    while (!sdr_url.empty() && sdr_url.back() == '/')
        sdr_url.pop_back();

    /* Default: all DSC frequencies */
    if (frequencies.empty())
        frequencies = get_default_dsc_frequencies();

    fprintf(stderr, "ubersdr server : %s\n", sdr_url.c_str());
    fprintf(stderr, "web UI         : http://localhost:%d/\n", web_port);
    fprintf(stderr, "frequencies    : %zu channels\n", frequencies.size());
    for (auto f : frequencies)
        fprintf(stderr, "  %s MHz (%s)\n", freq_mhz(f).c_str(), freq_band_name(f).c_str());

    /* ---- Install signal handlers ---- */
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    /* ---- Initialize networking ---- */
    ix::initNetSystem();

    /* ---- Create BroadcastHub + web server ---- */
    BroadcastHub hub;
    g_hub = &hub;

    ix::HttpServer web_server(web_port, "0.0.0.0");

    /* HTTP handler: serve the HTML page */
    web_server.setOnConnectionCallback(
        [&sdr_url, &frequencies](ix::HttpRequestPtr req,
                     std::shared_ptr<ix::ConnectionState> /*state*/)
        -> ix::HttpResponsePtr
        {
            std::string base_path;
            for (const auto &h : req->headers) {
                std::string key = h.first;
                for (auto &c : key) c = (char)tolower((unsigned char)c);
                if (key == "x-forwarded-prefix") {
                    base_path = strip_trailing_slash(h.second);
                    break;
                }
            }
            auto resp = std::make_shared<ix::HttpResponse>();
            resp->statusCode = 200;
            resp->description = "OK";
            resp->headers["Content-Type"] = "text/html; charset=utf-8";
            resp->body = make_html_page(sdr_url, frequencies, base_path);
            return resp;
        });

    /* WebSocket callback: handle browser client messages */
    web_server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> /*state*/,
           ix::WebSocket &client_ws,
           const ix::WebSocketMessagePtr &msg) {
            /* Handle audio-preview subscription control messages */
            if (msg->type == ix::WebSocketMessageType::Message &&
                !msg->binary && g_hub) {
                const std::string &body = msg->str;
                bool is_audio = body.find("\"audio_preview\"") != std::string::npos
                             || body.find("\"type\":\"audio_preview\"") != std::string::npos
                             || body.find("\"type\": \"audio_preview\"") != std::string::npos;
                if (is_audio) {
                    bool enable = body.find("\"enable\":true") != std::string::npos
                               || body.find("\"enable\": true") != std::string::npos;
                    if (enable) {
                        /* Parse frequency from JSON */
                        int64_t freq = 0;
                        auto pos = body.find("\"frequency\":");
                        if (pos != std::string::npos) {
                            freq = strtoll(body.c_str() + pos + 12, nullptr, 10);
                        }
                        if (freq > 0)
                            g_hub->sub_audio(&client_ws, freq);
                    } else {
                        g_hub->unsub_audio(&client_ws);
                    }
                }
            }
            /* On close, remove from audio subscription set */
            if (msg->type == ix::WebSocketMessageType::Close && g_hub) {
                g_hub->unsub_audio(&client_ws);
            }
        });

    hub.server = &web_server;

    auto res = web_server.listenAndStart();
    if (!res) {
        fprintf(stderr, "error: could not start web server on port %d\n", web_port);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "web UI         : http://localhost:%d/  (listening)\n", web_port);

    /* ---- Create ChannelManager ---- */
    /* The message callback is invoked from channel threads when a DSC
     * message is decoded.  We broadcast it to browser clients and log it. */
    ChannelManager mgr(sdr_url, [&hub](const DecodedMessage &dm) {
        /* Log to console */
        fprintf(stderr, "\n=== DSC MESSAGE on %.4f MHz ===\n%s\n",
                dm.frequency_hz / 1e6,
                dm.message.toString("\n").c_str());

        /* Enrich with MMSI lookups and log */
        std::string country = MMSI::getCountry(dm.message.m_selfId);
        if (!country.empty())
            fprintf(stderr, "Self ID country: %s\n", country.c_str());

        if (dm.message.m_hasAddress) {
            auto it = CoastStations.find(dm.message.m_address);
            if (it != CoastStations.end())
                fprintf(stderr, "Coast station  : %s\n", it->second.c_str());
        }

        fprintf(stderr, "ECC: %s | Errors: %d | RSSI: %.1f dB\n",
                dm.message.m_eccOk ? "OK" : "FAIL",
                dm.errors, dm.rssi);
        fprintf(stderr, "===\n\n");

        /* Broadcast to browser clients */
        hub.send_message(dm);
    });

    /* Wire audio callback so decoded PCM reaches the web UI audio preview */
    mgr.setAudioCallback([](int64_t freq, const std::vector<int16_t>& pcm) {
        if (g_hub) g_hub->send_audio(freq, pcm);
    });

    /* Add all frequency channels */
    for (auto f : frequencies)
        mgr.addChannel(f);

    /* Start all channel connection loops */
    mgr.startAll();

    fprintf(stderr, "all channels started, entering main loop\n");

    /* ---- Metrics broadcast thread (every 2 seconds) ---- */
    std::thread metrics_thread([&mgr, &hub]() {
        while (g_running) {
            /* Sleep 2 seconds in 100ms increments for responsive shutdown */
            for (int i = 0; i < 20 && g_running; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!g_running) break;

            auto metrics = mgr.getMetrics();
            hub.send_metrics(metrics);
        }
    });

    /* ---- Main loop: wait for signal ---- */
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    fprintf(stderr, "\nshutting down...\n");

    /* ---- Cleanup ---- */
    mgr.stopAll();

    if (metrics_thread.joinable())
        metrics_thread.join();

    web_server.stop();
    g_hub = nullptr;

    ix::uninitNetSystem();
    fprintf(stderr, "done.\n");
    return 0;
}
