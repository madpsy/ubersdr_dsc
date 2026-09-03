/* -*- c++ -*- */
/*
 * channel_manager.cpp — Multi-frequency DSC channel orchestrator
 *
 * Each channel independently:
 *   1. Generates a UUID v4 session ID
 *   2. POSTs /connection to register with ubersdr
 *   3. Opens a WebSocket for lossless audio (protocol version 4)
 *   4. Sends keepalive pings every 30 s
 *   5. Feeds decoded PCM to its own dsc_rx instance
 *   6. Reconnects on disconnect (10 s backoff)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "channel_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include <curl/curl.h>

#include <ixwebsocket/IXNetSystem.h>

#include "pcm_v4.hpp"

/* ================================================================== */
/* Helper functions (adapted from navtex_rx_from_ubersdr.cpp)          */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* UUID v4 generator                                                    */
/* ------------------------------------------------------------------ */
static std::string make_uuid4()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);

    /* Set version 4 and variant bits */
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%012llx",
             (unsigned)(hi >> 32),
             (unsigned)((hi >> 16) & 0xFFFF),
             (unsigned)(hi & 0xFFFF),
             (unsigned)(lo >> 48),
             (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

/* ------------------------------------------------------------------ */
/* libcurl helpers                                                      */
/* ------------------------------------------------------------------ */
struct CurlBuf {
    std::string data;
};

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<CurlBuf *>(userdata);
    buf->data.append(ptr, size * nmemb);
    return size * nmemb;
}

/* POST json_body to url, return HTTP response body in out_body.
 * Returns the HTTP status code, or -1 on transport error. */
static long http_post_json(const std::string &url,
                           const std::string &json_body,
                           std::string &out_body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf buf;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: ubersdr_dsc/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); /* allow self-signed */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = -1;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    else
        fprintf(stderr, "curl POST error: %s\n", curl_easy_strerror(res));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    out_body = buf.data;
    return http_code;
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                          */
/* ------------------------------------------------------------------ */
static std::string http_to_ws(const std::string &http_url)
{
    if (http_url.substr(0, 8) == "https://")
        return "wss://" + http_url.substr(8);
    if (http_url.substr(0, 7) == "http://")
        return "ws://" + http_url.substr(7);
    return http_url; /* pass through if already ws:// */
}

static std::string strip_slash(const std::string &s)
{
    if (!s.empty() && s.back() == '/')
        return s.substr(0, s.size() - 1);
    return s;
}

/* ------------------------------------------------------------------ */
/* Audio protocol version 4 frame decoder                               */
/* ------------------------------------------------------------------ */

/*
 * Version 4 replaced the zstd wrapper of versions 1-3 with a predictive
 * lossless codec and a variable-length header, so there is no compression
 * library here at all: pcm_v4.hpp is header-only and self-contained.
 *
 * The header carries the sample rate and the channel count itself — on a
 * resynchronisation packet, which is also the first packet any decoder will
 * accept — so nothing below assumes 10000 Hz or two channels.  Whatever the
 * server says is what reaches dsc_rx.
 */

struct PcmMeta {
    uint32_t sample_rate        = 0;
    uint8_t  channels           = 0;
    float    baseband_power     = 0.0f;  /* dBFS */
    /* Version 4 reports the noise POWER in the demodulator passband in dBFS,
     * not the dBFS/Hz density versions 1-3 sent -- the two differ by
     * 10*log10(passband), about 34 dB.  Nothing in this program reads either
     * field (the RSSI the UI shows is computed by dsc_rx from the samples), so
     * the migration changes no displayed number here; the name says which one
     * it is so that stays true if something ever does read it. */
    float    noise_dbfs         = 0.0f;  /* dBFS, passband noise power */
    bool     has_signal_quality = false;

    /* Consecutive decode failures, so a desynchronised stream reports itself
     * once rather than once per packet on every channel.  Cleared by the next
     * packet that decodes. */
    unsigned long decode_errors = 0;
};

/*
 * Decode one WebSocket binary frame.
 *
 * `dec` holds the stream's adaptation state and belongs to exactly one
 * WebSocket: the predictor is backward adaptive, so every packet has to reach
 * it (even one whose samples are then dropped) and it has to be reset when a
 * new socket opens.
 *
 * Fills meta from the packet header and the samples into pcm_le, host-order
 * int16.  Unlike versions 1-3 the codec emits host order already, so there is
 * no byte swap.
 */
static bool decode_pcm_v4_frame(ubersdr::PCMv4StreamDecoder &dec,
                                const void *frame, size_t frame_len,
                                PcmMeta &meta,
                                std::vector<int16_t> &pcm_le)
{
    const uint8_t *p = static_cast<const uint8_t *>(frame);

    if (ubersdr::PCMv4StreamDecoder::isZstdFrame(p, frame_len)) {
        /* A server older than 0.1.63 clamps the requested version to 1-3 and
         * answers with a zstd frame rather than refusing.  Say so plainly
         * instead of reporting "bad magic" on every packet. */
        if (++meta.decode_errors == 1)
            fprintf(stderr, "pcm v4: server sent a version 1-3 zstd frame; "
                            "it is older than 0.1.63 and cannot serve version 4\n");
        return false;
    }

    ubersdr::PCMv4Header h;
    std::string err;
    if (!dec.decode(p, frame_len, h, err)) {
        ++meta.decode_errors;
        if (meta.decode_errors == 1 || meta.decode_errors % 500 == 0)
            fprintf(stderr, "pcm v4: %s (%lu consecutive)\n",
                    err.c_str(), meta.decode_errors);
        return false;
    }
    meta.decode_errors = 0;

    meta.sample_rate = (uint32_t)h.sampleRate;
    meta.channels    = (uint8_t)h.channels;
    meta.baseband_power = h.basebandPower;
    meta.noise_dbfs     = h.noise;
    /* -999 is the "radiod reported nothing" sentinel.  Quality rides every
     * resynchronisation packet, and a resync is the first packet the decoder
     * accepts, so this is set from real data from the first frame on. */
    meta.has_signal_quality = (h.basebandPower > -998.0f);

    const int16_t *s = dec.samples();
    if (s == nullptr || h.sampleCount <= 0) {
        pcm_le.clear();
        return true;
    }
    pcm_le.assign(s, s + h.sampleCount);
    return true;
}

/* ================================================================== */
/* ChannelManager implementation                                        */
/* ================================================================== */

ChannelManager::ChannelManager(const std::string& ubersdr_base_url,
                               MessageCallback on_message)
    : m_base_url(strip_slash(ubersdr_base_url))
    , m_on_message(std::move(on_message))
{
    ix::initNetSystem();
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ChannelManager::~ChannelManager()
{
    stopAll();
    curl_global_cleanup();
    ix::uninitNetSystem();
}

/* ------------------------------------------------------------------ */
/* Channel management                                                   */
/* ------------------------------------------------------------------ */

void ChannelManager::setAudioCallback(AudioCallback cb)
{
    m_on_audio = std::move(cb);
}

void ChannelManager::addChannel(int64_t frequency_hz)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    /* Check for duplicate */
    for (const auto& ch : m_channels) {
        if (ch->frequency_hz == frequency_hz) {
            fprintf(stderr, "channel_manager: channel %lld Hz already exists\n",
                    (long long)frequency_hz);
            return;
        }
    }

    auto ch = std::make_unique<Channel>();
    ch->frequency_hz = frequency_hz;
    ch->session_id   = make_uuid4();
    ch->metrics.frequency_hz = frequency_hz;
    ch->metrics.enabled      = true;

    fprintf(stderr, "channel_manager: added channel %lld Hz (session %s)\n",
            (long long)frequency_hz, ch->session_id.c_str());

    m_channels.push_back(std::move(ch));
}

void ChannelManager::removeChannel(int64_t frequency_hz)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = std::find_if(m_channels.begin(), m_channels.end(),
        [frequency_hz](const std::unique_ptr<Channel>& ch) {
            return ch->frequency_hz == frequency_hz;
        });

    if (it == m_channels.end()) {
        fprintf(stderr, "channel_manager: channel %lld Hz not found\n",
                (long long)frequency_hz);
        return;
    }

    Channel* ch = it->get();

    /* Stop the channel if running */
    ch->should_run = false;
    if (ch->ws)
        ch->ws->stop();
    if (ch->channel_thread.joinable())
        ch->channel_thread.join();
    if (ch->keepalive_thread.joinable())
        ch->keepalive_thread.join();

    fprintf(stderr, "channel_manager: removed channel %lld Hz\n",
            (long long)frequency_hz);

    m_channels.erase(it);
}

void ChannelManager::enableChannel(int64_t frequency_hz, bool enabled)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    for (auto& ch : m_channels) {
        if (ch->frequency_hz == frequency_hz) {
            ch->enabled = enabled;
            {
                std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
                ch->metrics.enabled = enabled;
            }

            if (!enabled) {
                /* Stop the channel's WebSocket to disconnect */
                if (ch->ws)
                    ch->ws->stop();
            }

            fprintf(stderr, "channel_manager: channel %lld Hz %s\n",
                    (long long)frequency_hz, enabled ? "enabled" : "disabled");
            return;
        }
    }

    fprintf(stderr, "channel_manager: channel %lld Hz not found\n",
            (long long)frequency_hz);
}

/* ------------------------------------------------------------------ */
/* Start / stop all channels                                            */
/* ------------------------------------------------------------------ */

void ChannelManager::startAll()
{
    std::lock_guard<std::mutex> lk(m_mutex);

    for (auto& ch : m_channels) {
        if (ch->should_run)
            continue;  /* already running */

        ch->should_run = true;

        /* Launch the channel's connection loop in its own thread */
        ch->channel_thread = std::thread([this, raw = ch.get()]() {
            channelLoop(raw);
        });
    }

    fprintf(stderr, "channel_manager: started %zu channel(s)\n", m_channels.size());
}

void ChannelManager::stopAll()
{
    /* Signal all channels to stop */
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& ch : m_channels) {
            ch->should_run = false;
            if (ch->ws)
                ch->ws->stop();
        }
    }

    /* Join all threads (outside the lock to avoid deadlock) */
    /* We take a snapshot of raw pointers since we only need to join */
    std::vector<Channel*> channels;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& ch : m_channels)
            channels.push_back(ch.get());
    }

    for (auto* ch : channels) {
        if (ch->channel_thread.joinable())
            ch->channel_thread.join();
        if (ch->keepalive_thread.joinable())
            ch->keepalive_thread.join();
    }

    fprintf(stderr, "channel_manager: all channels stopped\n");
}

/* ------------------------------------------------------------------ */
/* Query state                                                          */
/* ------------------------------------------------------------------ */

std::vector<ChannelMetrics> ChannelManager::getMetrics() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<ChannelMetrics> result;
    result.reserve(m_channels.size());

    for (const auto& ch : m_channels) {
        std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
        result.push_back(ch->metrics);
    }

    return result;
}

std::vector<DecodedMessage> ChannelManager::getMessages(int max_count) const
{
    std::lock_guard<std::mutex> lk(m_mutex);

    if (max_count <= 0 || max_count >= (int)m_messages.size())
        return m_messages;

    /* Return the most recent max_count messages */
    return std::vector<DecodedMessage>(
        m_messages.end() - max_count, m_messages.end());
}

size_t ChannelManager::getMessageCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_messages.size();
}

/* ------------------------------------------------------------------ */
/* Register session with ubersdr (POST /connection)                     */
/* ------------------------------------------------------------------ */

bool ChannelManager::registerSession(Channel* ch)
{
    std::string conn_url = m_base_url + "/connection";
    std::string body = "{\"user_session_id\":\"" + ch->session_id + "\"}";
    std::string resp;

    long code = http_post_json(conn_url, body, resp);
    if (code < 0) {
        fprintf(stderr, "channel %lld Hz: could not reach %s\n",
                (long long)ch->frequency_hz, conn_url.c_str());
        return false;
    }

    fprintf(stderr, "channel %lld Hz: POST /connection → HTTP %ld\n",
            (long long)ch->frequency_hz, code);

    if (code == 403) {
        fprintf(stderr, "channel %lld Hz: connection rejected (password required?): %s\n",
                (long long)ch->frequency_hz, resp.c_str());
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Per-channel connection loop                                          */
/* ------------------------------------------------------------------ */

void ChannelManager::channelLoop(Channel* ch)
{
    fprintf(stderr, "channel %lld Hz: connection loop started\n",
            (long long)ch->frequency_hz);

    /* Per-channel decoder state — persists across reconnects */
    PcmMeta              meta;
    std::vector<int16_t> pcm_le;
    bool                 warned_channels = false;

    /* The version 4 stream decoder holds this channel's adaptation state.  It
     * belongs to one WebSocket, so it is reset at the top of every session
     * below; unlike dsc_rx it must NOT carry state across reconnects. */
    ubersdr::PCMv4StreamDecoder v4;

    /* dsc_rx has to exist before the first frame arrives, so it is built at the
     * rate mode=iq has always delivered.  That is only a starting value: the
     * version 4 header carries the rate and the channel count on every
     * resynchronisation packet, and a resync is the first packet the decoder
     * will accept, so the loop below rebuilds dsc_rx from the wire the moment
     * the server says anything else.  Nothing downstream assumes 10 kHz. */
    int                  current_sample_rate = 10000;

    /* Short label used in debug log lines, e.g. "16.8060 MHz" */
    char ch_label_buf[32];
    snprintf(ch_label_buf, sizeof(ch_label_buf), "%.4f MHz",
             ch->frequency_hz / 1e6);
    std::string ch_label(ch_label_buf);

    /* Create the decoder with default sample rate; will be recreated if rate changes */
    auto message_cb = [this, ch](const DSCMessage& msg, int errors, float rssi) {
        DecodedMessage dm{msg, errors, rssi, ch->frequency_hz, std::time(nullptr)};

        /* Update per-channel metrics */
        {
            std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
            ch->metrics.message_count++;
            if (msg.m_valid && msg.m_eccOk)
                ch->metrics.valid_message_count++;
            ch->metrics.total_errors += errors;
            ch->metrics.rssi_sum += rssi;
            ch->metrics.rssi_count++;
            ch->metrics.last_message_time = dm.received_at;
        }

        /* Store in shared message list and invoke callback */
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_messages.push_back(dm);
        }

        if (m_on_message)
            m_on_message(dm);
    };

    ch->decoder = std::make_unique<dsc_rx>(current_sample_rate, message_cb, ch_label);

    /* Build WebSocket base URL */
    std::string ws_base = http_to_ws(m_base_url);

    /* Outer reconnect loop */
    while (ch->should_run) {

        /* If disabled, sleep and check periodically */
        if (!ch->enabled) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        /* 1. Register session with ubersdr */
        if (!registerSession(ch)) {
            /* Wait before retrying */
            for (int i = 0; i < 10 && ch->should_run; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        /* 2. Build WebSocket URL
         * IQ mode: tune to DSC center frequency, 10 kHz bandwidth.
         * ubersdr delivers CS16 (little-endian interleaved I/Q int16) at 10000 Hz.
         * The DSC mark tone (+85 Hz) and space tone (-85 Hz) are present as
         * complex exponentials relative to the tuned centre. */
        int64_t dial_hz = ch->frequency_hz;  // tune to DSC centre directly

        char ws_url[1024];
        snprintf(ws_url, sizeof(ws_url),
                 "%s/ws?frequency=%lld&mode=iq"
                 "&format=pcm-zstd&version=4"
                 "&user_session_id=%s",
                 ws_base.c_str(), (long long)dial_hz, ch->session_id.c_str());

        fprintf(stderr, "channel %lld Hz: connecting to %s\n",
                (long long)ch->frequency_hz, ws_url);

        /* 3. Open WebSocket */
        std::atomic<bool> session_done{false};

        /* A new socket is a new stream: the version 4 predictor is backward
         * adaptive, so carrying the previous session's taps into this one would
         * decode the first packets as noise.  Reset before any frame can
         * arrive, i.e. before the socket is created. */
        v4.reset();

        ch->ws = std::make_unique<ix::WebSocket>();
        ch->ws->setUrl(ws_url);
        ch->ws->setHandshakeTimeout(10);

        /* Allow self-signed TLS certificates */
        ix::SocketTLSOptions tls;
        tls.caFile = "NONE";
        ch->ws->setTLSOptions(tls);

        ch->ws->setOnMessageCallback(
            [&, ch](const ix::WebSocketMessagePtr &msg) {
                switch (msg->type) {

                case ix::WebSocketMessageType::Open:
                    fprintf(stderr, "channel %lld Hz: WebSocket connected\n",
                            (long long)ch->frequency_hz);
                    ch->connected = true;
                    {
                        std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
                        ch->metrics.connected = true;
                    }
                    /* Request initial status */
                    ch->ws->sendText("{\"type\":\"get_status\"}");
                    break;

                case ix::WebSocketMessageType::Close:
                    fprintf(stderr, "channel %lld Hz: WebSocket closed: %s\n",
                            (long long)ch->frequency_hz,
                            msg->closeInfo.reason.c_str());
                    ch->connected = false;
                    {
                        std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
                        ch->metrics.connected = false;
                    }
                    session_done = true;
                    break;

                case ix::WebSocketMessageType::Error:
                    fprintf(stderr, "channel %lld Hz: WebSocket error: %s\n",
                            (long long)ch->frequency_hz,
                            msg->errorInfo.reason.c_str());
                    session_done = true;
                    break;

                case ix::WebSocketMessageType::Message:
                    if (msg->binary) {
                        /* Binary frame: audio protocol version 4.
                         * Every frame must reach the stream decoder, even one
                         * whose samples are then dropped — it is the only thing
                         * keeping this side's predictor in step with the
                         * server's. */
                        if (!decode_pcm_v4_frame(v4, msg->str.data(), msg->str.size(),
                                                 meta, pcm_le))
                            break;

                        /* Rebuild dsc_rx if the rate the server reports differs
                         * from the one it was built at.  On a version 4 stream
                         * this is authoritative from the very first frame,
                         * because that frame is a resynchronisation packet and
                         * carries the rate. */
                        if (meta.sample_rate != 0 &&
                            (int)meta.sample_rate != current_sample_rate) {
                            fprintf(stderr, "channel %lld Hz: sample rate: %u Hz\n",
                                    (long long)ch->frequency_hz, meta.sample_rate);
                            current_sample_rate = (int)meta.sample_rate;
                            ch->decoder = std::make_unique<dsc_rx>(
                                current_sample_rate, message_cb, ch_label);
                        }

                        /* Feed IQ samples to the DSC decoder.
                         * mode=iq delivers interleaved I/Q, so the header says
                         * two channels and pcm_le holds (I, Q, I, Q, ...).
                         * process() takes the number of IQ *pairs*, not the raw
                         * int16 count, so the divisor is the channel count the
                         * header reported rather than a constant. */
                        if (!pcm_le.empty()) {
                            if (meta.channels != 2) {
                                if (!warned_channels) {
                                    warned_channels = true;
                                    fprintf(stderr,
                                            "channel %lld Hz: server reports %u channels, "
                                            "not the interleaved I/Q dsc_rx needs — "
                                            "dropping samples\n",
                                            (long long)ch->frequency_hz,
                                            (unsigned)meta.channels);
                                }
                                break;
                            }
                            warned_channels = false;

                            int nb_pairs = (int)(pcm_le.size() / meta.channels);
                            ch->decoder->process(pcm_le.data(), nb_pairs);

                            /* Forward raw IQ to audio callback (for web UI preview) */
                            if (m_on_audio)
                                m_on_audio(ch->frequency_hz, pcm_le);
                        }
                    }
                    /* Text frames (JSON status/pong) are ignored */
                    break;

                default:
                    break;
                }
            });

        ch->ws->start();

        /* 4. Keepalive thread: send ping every 30 seconds */
        ch->keepalive_thread = std::thread([ch, &session_done]() {
            while (!session_done && ch->should_run) {
                for (int i = 0; i < 30 && !session_done && ch->should_run; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!session_done && ch->should_run && ch->connected && ch->ws)
                    ch->ws->sendText("{\"type\":\"ping\"}");
            }
        });

        /* 5. Wait until this session ends */
        while (!session_done && ch->should_run)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        /* Clean up this session */
        ch->ws->stop();
        if (ch->keepalive_thread.joinable())
            ch->keepalive_thread.join();
        ch->ws.reset();

        ch->connected = false;
        {
            std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
            ch->metrics.connected = false;
        }

        if (!ch->should_run)
            break;

        /* 6. Reconnect backoff: wait 10 seconds */
        {
            std::lock_guard<std::mutex> mlk(ch->metrics_mutex);
            ch->metrics.reconnect_count++;
        }

        fprintf(stderr, "channel %lld Hz: reconnecting in 10 seconds...\n",
                (long long)ch->frequency_hz);

        for (int i = 10; i > 0 && ch->should_run; --i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

    } /* end reconnect loop */

    fprintf(stderr, "channel %lld Hz: connection loop exited\n",
            (long long)ch->frequency_hz);
}
