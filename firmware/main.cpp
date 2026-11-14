#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <cmath>
#include <cstdio>
#include "Telemetry.h"

#ifndef HEIROWECU_DEMO
#define HEIROWECU_DEMO 0
#endif
#ifndef HEIROWECU_SERIAL_FEED
#define HEIROWECU_SERIAL_FEED 0
#endif
#ifndef ELM_BAUD
#define ELM_BAUD 38400
#endif

namespace {

extern const char Dashboard[] asm("_binary_dashboard_index_html_start");
WebServer server(80);
telemetry::Store readings;
bool listening = false;

bool due(uint32_t now, uint32_t when) {
    return static_cast<int32_t>(now - when) >= 0;
}

#if !HEIROWECU_DEMO && !HEIROWECU_SERIAL_FEED
class ElmPoller {
public:
    void begin() {
        port_.begin(ELM_BAUD, SERIAL_8N1, ELM_RX_PIN, ELM_TX_PIN);
        restart(millis());
    }

    void tick(uint32_t now) {
        for (unsigned i = 0; i < reply_.size() && port_.available(); ++i) {
            const auto c = static_cast<char>(port_.read());
            if (!pending_) continue;
            if (c == '>') {
                complete(now);
            } else if (used_ < reply_.size()) {
                reply_[used_++] = c;
            } else {
                overflow_ = true;
            }
        }
        if (pending_) {
            if (due(now, deadline_)) restart(now);
            return;
        }
        if (!due(now, nextRequest_)) return;
        switch (phase_) {
            case Phase::Configure: send(Setup[setupIndex_], now, 1800); return;
            case Phase::Probe: send("0100", now, 18000); return;
            case Phase::ProbeBarometer: send("0120", now, 1800); return;
            case Phase::Poll: poll(now); return;
        }
    }

private:
    enum class Phase { Configure, Probe, ProbeBarometer, Poll };
    static constexpr std::array<const char*, 6> Setup{"ATZ", "ATE0", "ATL0", "ATH0", "ATS1", "ATSP0"};
    static constexpr std::array<uint8_t, 10> Order{0x0C, 0x0B, 0x0C, 0x0F, 0x0C, 0x0B, 0x05, 0x0C, 0x0B, 0x33};
    HardwareSerial port_{1};
    std::array<char, 512> reply_{};
    size_t used_ = 0, setupIndex_ = 0, orderIndex_ = 0;
    Phase phase_ = Phase::Configure;
    uint32_t nextRequest_ = 0, deadline_ = 0, lastTelemetry_ = 0, lastBarometer_ = 0, pids_ = 0;
    uint8_t activePid_ = 0;
    bool pending_ = false, overflow_ = false, hasBarometer_ = false, sampledBarometer_ = false;

    void restart(uint32_t now) {
        readings.clear();
        phase_ = Phase::Configure;
        pending_ = false;
        setupIndex_ = orderIndex_ = 0;
        nextRequest_ = now + 1000;
        hasBarometer_ = sampledBarometer_ = false;
    }

    void send(const char* command, uint32_t now, uint32_t timeout) {
        used_ = 0;
        overflow_ = false;
        pending_ = true;
        deadline_ = now + timeout;
        port_.print(command);
        port_.write('\r');
    }

    void complete(uint32_t now) {
        pending_ = false;
        nextRequest_ = now + 20;
        if (overflow_) {
            restart(now);
            return;
        }
        const std::string_view reply(reply_.data(), used_);
        switch (phase_) {
            case Phase::Configure:
                // Reset returns a banner; the remaining commands must acknowledge.
                if (setupIndex_ != 0 && !telemetry::adapterOk(reply)) {
                    restart(now);
                    return;
                }
                if (++setupIndex_ == Setup.size()) phase_ = Phase::Probe;
                return;
            case Phase::Probe:
                if (!telemetry::mode01Payload(reply, 0, 4, pids_)) {
                    restart(now);
                    return;
                }
                phase_ = telemetry::supported(pids_, 0, 0x20) ? Phase::ProbeBarometer : Phase::Poll;
                lastTelemetry_ = now;
                return;
            case Phase::ProbeBarometer: {
                uint32_t more = 0;
                hasBarometer_ = telemetry::mode01Payload(reply, 0x20, 4, more) && telemetry::supported(more, 0x20, 0x33);
                phase_ = Phase::Poll;
                lastTelemetry_ = now;
                return;
            }
            case Phase::Poll:
                if (telemetry::updateMode01(readings, activePid_, reply, now)) lastTelemetry_ = now;
                return;
        }
    }

    void poll(uint32_t now) {
        if (now - lastTelemetry_ >= 4500) {
            restart(now);
            return;
        }
        for (size_t i = 0; i < Order.size(); ++i) {
            const auto candidate = Order[orderIndex_];
            orderIndex_ = (orderIndex_ + 1) % Order.size();
            if (candidate == 0x33) {
                if (!hasBarometer_ || (sampledBarometer_ && now - lastBarometer_ < 2000)) continue;
                sampledBarometer_ = true;
                lastBarometer_ = now;
            } else if (!telemetry::supported(pids_, 0, candidate)) {
                continue;
            }
            activePid_ = candidate;
            char command[5];
            std::snprintf(command, sizeof(command), "01%02X", static_cast<unsigned>(candidate));
            send(command, now, 1400);
            return;
        }
        nextRequest_ = now + 400;
    }
};

ElmPoller elm;
#endif

bool password(String& value) {
    Preferences settings;
    if (!settings.begin("heirowecu", false)) return false;
    value = settings.getString("wifi", "");
    if (value.length() != 16) {
        // Wi-Fi radio is running before reading the hardware RNG.
        constexpr char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        char key[17]{};
        for (size_t i = 0; i < sizeof(random); ++i) key[i] = alphabet[random[i] & 31U];
        if (settings.putString("wifi", key) != 16) {
            settings.end();
            return false;
        }
        value = key;
    }
    settings.end();
    return true;
}

void responseHeaders() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
}

void dashboard() {
    responseHeaders();
    server.sendHeader("Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    server.send_P(200, PSTR("text/html; charset=utf-8"), Dashboard);
}

void liveFrame() {
    responseHeaders();
    const auto frame = readings.snapshot(millis());
    if (frame.available == 0) {
        server.send(204);
        return;
    }
    char json[192];
    if (telemetry::writeJson(frame, HEIROWECU_DEMO != 0, json, sizeof(json)) == 0) {
        server.send(500, "text/plain", "Telemetry unavailable");
        return;
    }
    server.send(200, "application/json", json);
}

#if HEIROWECU_SERIAL_FEED
void readSerialFeed(uint32_t now) {
    static std::array<char, 96> line{};
    static size_t used = 0;
    static bool overflow = false;
    for (unsigned i = 0; i < 256 && Serial.available(); ++i) {
        const auto c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n') {
            telemetry::Frame frame;
            if (!overflow && telemetry::parseSerialFrame({line.data(), used}, frame)) readings.replaceDisplay(frame, now);
            overflow = false;
            used = 0;
        } else if (used < line.size()) {
            line[used++] = c;
        } else {
            overflow = true;
        }
    }
}
#endif

#if HEIROWECU_DEMO
void updateDemo(uint32_t now) {
    static uint32_t nextFrame = 0;
    if (!due(now, nextFrame)) return;
    nextFrame = now + 100;
    const auto slow = sinf(static_cast<float>(now % 15000) / 15000.0f * TWO_PI);
    const auto fast = sinf(static_cast<float>(now % 6500) / 6500.0f * TWO_PI);
    const telemetry::Frame frame{0, 15, {3000.0f + fast * 1000.0f, 6.0f + fast * 9.0f, 34.0f + slow * 5.0f, 91.0f + slow * 2.0f}};
    readings.replaceDisplay(frame, now);
}
#endif

} // namespace

void setup() {
    Serial.begin(115200);
    char ssid[23];
    const auto suffix = static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFFFFULL);
    std::snprintf(ssid, sizeof(ssid), HEIROWECU_DEMO ? "heirowECU-DEMO-%06X" : "heirowECU-%06X", suffix);
    const IPAddress local(192, 168, 4, 1), mask(255, 255, 255, 0);
    String key;
    if (!WiFi.mode(WIFI_AP) || !password(key) || !WiFi.softAPConfig(local, local, mask) ||
        !WiFi.softAP(ssid, key.c_str(), 6, false, 4)) {
        Serial.println("heirowECU: unable to start protected access point");
        return;
    }
    Serial.println("heirowECU Wi-Fi bridge");
    Serial.printf("SSID: %s\nPassword: %s\nDashboard: http://192.168.4.1/\n", ssid, key.c_str());
    server.on("/", HTTP_GET, dashboard);
    server.on("/v1/telemetry", HTTP_GET, liveFrame);
    server.onNotFound([] {
        responseHeaders();
        server.send(404, "text/plain", "Not found");
    });
    server.begin();
    listening = true;
#if !HEIROWECU_DEMO && !HEIROWECU_SERIAL_FEED
    elm.begin();
#endif
}

void loop() {
    if (!listening) {
        delay(50);
        return;
    }
    const auto now = millis();
#if HEIROWECU_DEMO
    updateDemo(now);
#elif HEIROWECU_SERIAL_FEED
    readSerialFeed(now);
#else
    elm.tick(now);
#endif
    server.handleClient();
    delay(1);
}
