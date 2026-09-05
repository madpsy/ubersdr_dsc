/* -*- c++ -*- */
/*
 * channel_manager.h — Multi-frequency DSC channel orchestrator
 *
 * Manages N parallel ubersdr WebSocket connections, each with its own
 * DSC decoder instance, keepalive thread, and independent reconnection logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "dsc_rx.h"
#include "dsc_message.h"

/* ------------------------------------------------------------------ */
/* Per-channel metrics (snapshot)                                       */
/* ------------------------------------------------------------------ */
struct ChannelMetrics {
    int64_t frequency_hz      = 0;
    int     message_count     = 0;
    int     valid_message_count = 0;
    int     total_errors      = 0;
    double  rssi_sum          = 0.0;
    int     rssi_count        = 0;
    time_t  last_message_time = 0;   // 0 if never
    bool    enabled           = true;
    bool    connected         = false;
    int     reconnect_count   = 0;
};

/* ------------------------------------------------------------------ */
/* Decoded message with reception metadata                              */
/* ------------------------------------------------------------------ */
struct DecodedMessage {
    DSCMessage message;
    int        errors        = 0;
    float      rssi          = 0.0f;
    int64_t    frequency_hz  = 0;
    time_t     received_at   = 0;
};

/* ------------------------------------------------------------------ */
/* Reduced-depth I/Q request (min_margin)                               */
/*                                                                      */
/* The server sends lossless I/Q to anything that does not ask          */
/* otherwise, at roughly twice the bytes.  min_margin asks it instead   */
/* to requantise each packet only as far as it can while keeping the    */
/* quantisation floor this many dB below the band's own noise floor,    */
/* which is a request the server can honour identically on a quiet 6 m  */
/* band and on medium wave, where a fixed bit depth cannot.             */
/*                                                                      */
/* 26 dB is the measured transparent setting and is what every shipped  */
/* ubersdr client defaults to.  0 asks for lossless.  Anything else has */
/* to be 15-60: the server silently clamps out-of-range values, so the  */
/* range is checked here instead, where it can be reported.             */
/* ------------------------------------------------------------------ */
static const int kMinMarginDefaultDB = 26;
static const int kMinMarginMinDB     = 15;
static const int kMinMarginMaxDB     = 60;

/* ------------------------------------------------------------------ */
/* ChannelManager                                                       */
/* ------------------------------------------------------------------ */
class ChannelManager {
public:
    using MessageCallback = std::function<void(const DecodedMessage&)>;
    using AudioCallback = std::function<void(int64_t freq, const std::vector<int16_t>& pcm)>;

    ChannelManager(const std::string& ubersdr_base_url, MessageCallback on_message,
                   int min_margin_db = kMinMarginDefaultDB);
    ~ChannelManager();

    // ---- Channel management ----
    void addChannel(int64_t frequency_hz);
    void removeChannel(int64_t frequency_hz);
    void enableChannel(int64_t frequency_hz, bool enabled);
    void setAudioCallback(AudioCallback cb);

    // ---- Start / stop all channels ----
    void startAll();
    void stopAll();

    // ---- Query state ----
    std::vector<ChannelMetrics> getMetrics() const;
    std::vector<DecodedMessage> getMessages(int max_count = 0) const;  // 0 = all
    size_t getMessageCount() const;

private:
    /* Per-channel state — each channel owns its own decoder, WebSocket,
     * keepalive thread, and connection-loop thread. */
    struct Channel {
        int64_t                    frequency_hz  = 0;
        std::string                session_id;        // unique UUID per channel
        std::unique_ptr<dsc_rx>    decoder;
        std::unique_ptr<ix::WebSocket> ws;
        std::thread                keepalive_thread;
        std::atomic<bool>          connected{false};
        std::atomic<bool>          should_run{false};
        std::atomic<bool>          enabled{true};
        std::thread                channel_thread;     // runs channelLoop()
        ChannelMetrics             metrics;
        mutable std::mutex         metrics_mutex;      // protects metrics
    };

    std::string     m_base_url;
    int             m_min_margin_db;   // 0 = lossless, otherwise 15-60
    MessageCallback m_on_message;
    AudioCallback   m_on_audio;

    mutable std::mutex                       m_mutex;       // protects m_channels & m_messages
    std::vector<std::unique_ptr<Channel>>    m_channels;
    std::vector<DecodedMessage>              m_messages;     // shared message store

    // ---- Internal helpers ----

    // Per-channel connection loop (runs in channel_thread)
    void channelLoop(Channel* ch);

    // Register session with ubersdr (POST /connection)
    bool registerSession(Channel* ch);
};

#endif /* CHANNEL_MANAGER_H */
