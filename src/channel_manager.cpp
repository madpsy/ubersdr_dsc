/* -*- c++ -*- */
/*
 * channel_manager.cpp — Multi-frequency DSC channel orchestrator
 *
 * Each channel independently:
 *   1. Generates a UUID v4 session ID
 *   2. POSTs /connection to register with ubersdr
 *   3. Opens a WebSocket for PCM-zstd audio
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

#include <arpa/inet.h>   /* ntohs */

#include <curl/curl.h>
#include <zstd.h>

#include <ixwebsocket/IXNetSystem.h>

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
/* Little-endian readers                                                */
/* ------------------------------------------------------------------ */
static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* ------------------------------------------------------------------ */
/* PCM-zstd frame decoder                                               */
/* ------------------------------------------------------------------ */

/*
 * Header magic values (little-endian uint16):
 *   Full header:    0x5043  ("CP" on wire)
 *   Minimal header: 0x504D  ("MP" on wire)
 */
static const uint16_t MAGIC_FULL    = 0x5043;
static const uint16_t MAGIC_MINIMAL = 0x504D;

/* Full header v1 = 29 bytes, v2 = 37 bytes, minimal = 13 bytes */
static const size_t FULL_HDR_V1 = 29;
static const size_t FULL_HDR_V2 = 37;
static const size_t MIN_HDR     = 13;

struct PcmMeta {
    uint32_t sample_rate        = 0;
    uint8_t  channels           = 0;
    float    baseband_power     = 0.0f;  /* dBFS, v2 header only */
    float    noise_density      = 0.0f;  /* dBFS/Hz, v2 header only */
    bool     has_signal_quality = false;
};

/*
 * Decompress one zstd-compressed PCM frame.
 * Fills meta (from full header), appends little-endian int16 samples to pcm_le.
 * Returns true on success.
 */
static bool decode_pcm_zstd_frame(const void *compressed, size_t compressed_len,
                                   std::vector<uint8_t> &decomp_buf,
                                   PcmMeta &meta,
                                   std::vector<int16_t> &pcm_le)
{
    /* Decompress */
    size_t frame_size = ZSTD_getFrameContentSize(compressed, compressed_len);
    if (frame_size == ZSTD_CONTENTSIZE_ERROR ||
        frame_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        /* Try a fixed upper bound */
        frame_size = compressed_len * 8;
    }
    if (decomp_buf.size() < frame_size)
        decomp_buf.resize(frame_size);

    size_t actual = ZSTD_decompress(decomp_buf.data(), decomp_buf.size(),
                                    compressed, compressed_len);
    if (ZSTD_isError(actual)) {
        fprintf(stderr, "zstd error: %s\n", ZSTD_getErrorName(actual));
        return false;
    }

    const uint8_t *p   = decomp_buf.data();
    const uint8_t *end = p + actual;

    if (actual < 2) return false;

    /* Magic is little-endian uint16 */
    uint16_t magic = read_le16(p);
    size_t   hdr_size = 0;

    if (magic == MAGIC_FULL) {
        /* Full header: version byte at offset 2 determines size */
        if (actual < 3) return false;
        uint8_t version = p[2];
        hdr_size = (version >= 2) ? FULL_HDR_V2 : FULL_HDR_V1;
        if (actual < hdr_size) return false;

        /* sample_rate at offset 20 (little-endian uint32) */
        meta.sample_rate = read_le32(p + 20);
        /* channels at offset 24 (uint8) */
        meta.channels    = p[24];

        /* v2: baseband_power at offset 25, noise_density at offset 29 (LE float32) */
        if (version >= 2) {
            uint32_t bb_bits, nd_bits;
            bb_bits = (uint32_t)p[25] | ((uint32_t)p[26]<<8) | ((uint32_t)p[27]<<16) | ((uint32_t)p[28]<<24);
            nd_bits = (uint32_t)p[29] | ((uint32_t)p[30]<<8) | ((uint32_t)p[31]<<16) | ((uint32_t)p[32]<<24);
            memcpy(&meta.baseband_power, &bb_bits, 4);
            memcpy(&meta.noise_density,  &nd_bits, 4);
            meta.has_signal_quality = true;
        } else {
            meta.has_signal_quality = false;
        }

    } else if (magic == MAGIC_MINIMAL) {
        hdr_size = MIN_HDR;
        if (actual < hdr_size) return false;
        /* Minimal header reuses last known sample_rate/channels */
    } else {
        fprintf(stderr, "unknown PCM magic: 0x%04x\n", magic);
        return false;
    }

    /* PCM payload: big-endian int16, convert to host (little-endian) */
    const uint8_t *pcm_start = p + hdr_size;
    size_t         pcm_bytes = (size_t)(end - pcm_start);
    size_t         n_samples = pcm_bytes / 2;

    pcm_le.resize(n_samples);
    for (size_t i = 0; i < n_samples; i++) {
        int16_t be;
        memcpy(&be, pcm_start + i * 2, 2);
        pcm_le[i] = (int16_t)ntohs((uint16_t)be);
    }
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
    std::vector<uint8_t> decomp_buf;
    std::vector<int16_t> pcm_le;
    int                  current_sample_rate = 12000; /* default, updated from PCM header */

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

    ch->decoder = std::make_unique<dsc_rx>(current_sample_rate, message_cb);

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
         * dial_hz = frequency_hz - 500 (USB mode, DSC audio center at 500 Hz) */
        int64_t dial_hz = ch->frequency_hz - 500;

        char ws_url[1024];
        snprintf(ws_url, sizeof(ws_url),
                 "%s/ws?frequency=%lld&mode=usb"
                 "&bandwidthLow=50&bandwidthHigh=2700"
                 "&format=pcm-zstd&version=2"
                 "&user_session_id=%s",
                 ws_base.c_str(), (long long)dial_hz, ch->session_id.c_str());

        fprintf(stderr, "channel %lld Hz: connecting to %s\n",
                (long long)ch->frequency_hz, ws_url);

        /* 3. Open WebSocket */
        std::atomic<bool> session_done{false};

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
                        /* Binary frame: PCM-zstd compressed audio */
                        if (!decode_pcm_zstd_frame(msg->str.data(), msg->str.size(),
                                                   decomp_buf, meta, pcm_le))
                            break;

                        /* Recreate decoder if sample rate changed */
                        if (meta.sample_rate != 0 &&
                            (int)meta.sample_rate != current_sample_rate) {
                            fprintf(stderr, "channel %lld Hz: sample rate: %u Hz\n",
                                    (long long)ch->frequency_hz, meta.sample_rate);
                            current_sample_rate = (int)meta.sample_rate;
                            ch->decoder = std::make_unique<dsc_rx>(
                                current_sample_rate, message_cb);
                        }

                        /* Feed PCM samples to the DSC decoder */
                        if (!pcm_le.empty()) {
                            ch->decoder->process(pcm_le.data(), (int)pcm_le.size());

                            /* Forward decoded PCM to audio callback (for web UI preview) */
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
