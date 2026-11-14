#include "Telemetry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace telemetry {
namespace {

constexpr auto indexOf(Signal signal) { return static_cast<size_t>(signal); }
constexpr uint64_t MaxSequence = 9007199254740991ULL;
constexpr float PsiPerKpa = 0.1450377377f;
constexpr std::array<std::string_view, DisplaySignals> DisplayKeys{"rpm", "boost_psi", "iat_c", "coolant_c"};

bool readScalarsWithin(Signal signal, float value) {
    switch (signal) {
        case Signal::Rpm: return value >= 0 && value <= 16400;
        case Signal::Boost: return value >= -35 && value <= 300;
        case Signal::Intake:
        case Signal::Coolant: return value >= -40 && value <= 215;
        case Signal::Map:
        case Signal::Barometer: return value > 0 && value <= 255;
        default: return false;
    }
}

std::string_view trim(std::string_view line) {
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    return line.substr(start, line.find_last_not_of(" \t") - start + 1);
}

template <class Callback> void forEachLine(std::string_view reply, const Callback& callback) {
    while (!reply.empty()) {
        const auto end = reply.find_first_of("\r\n");
        callback(trim(reply.substr(0, end)));
        if (end == std::string_view::npos) return;
        reply.remove_prefix(end + 1);
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool hexLine(std::string_view line, uint8_t length, uint64_t& result) {
    uint8_t digits = 0;
    result = 0;
    for (const char c : line) {
        if (c == ' ' || c == '\t') continue;
        const auto digit = hexValue(c);
        if (digit < 0 || digits == length * 2) return false;
        result = (result << 4) | static_cast<uint64_t>(digit);
        ++digits;
    }
    return digits == length * 2;
}

bool number(std::string_view field, float& value) {
    // A decimal field is bounded before copying, independent of line endings or locale.
    if (field.empty() || field.size() > 16 || field.find_first_not_of("0123456789-.") != std::string_view::npos)
        return false;
    char text[17]{};
    std::memcpy(text, field.data(), field.size());
    char* end = nullptr;
    value = std::strtof(text, &end);
    return end == text + field.size() && std::isfinite(value);
}

} // namespace

void Store::update(Signal signal, float value, uint32_t now) {
    if (!readScalarsWithin(signal, value)) return;
    readings_[indexOf(signal)] = {value, now, true};
    ++generation_;
}

void Store::replaceDisplay(const Frame& frame, uint32_t now) {
    for (size_t i = 0; i < DisplaySignals; ++i)
        readings_[i] = {frame.values[i], now, (frame.available & (1U << i)) != 0};
    ++generation_;
}

void Store::clear() {
    readings_.fill({});
}

Frame Store::snapshot(uint32_t now) {
    const auto fresh = [&](Signal signal, uint32_t lifetime = SignalLifetimeMs) {
        const auto& reading = readings_[indexOf(signal)];
        return reading.valid && now - reading.at < lifetime;
    };
    const auto value = [&](Signal signal) { return readings_[indexOf(signal)].value; };
    Frame frame;
    const auto add = [&](Signal signal, float reading) {
        const auto index = indexOf(signal);
        frame.available |= static_cast<uint8_t>(1U << index);
        frame.values[index] = reading;
    };
    for (const auto signal : {Signal::Rpm, Signal::Intake, Signal::Coolant})
        if (fresh(signal)) add(signal, value(signal));
    if (fresh(Signal::Boost))
        add(Signal::Boost, value(Signal::Boost));
    else if (fresh(Signal::Map) && fresh(Signal::Barometer, BarometerLifetimeMs))
        add(Signal::Boost, (value(Signal::Map) - value(Signal::Barometer)) * PsiPerKpa);

    if (frame.available != lastAvailable_ || generation_ != publishedGeneration_) {
        sequence_ = sequence_ == MaxSequence ? 0 : sequence_ + 1;
        lastAvailable_ = frame.available;
        publishedGeneration_ = generation_;
    }
    frame.sequence = sequence_;
    return frame;
}

bool adapterOk(std::string_view reply) {
    bool result = false;
    forEachLine(reply, [&](std::string_view line) { result |= line == "OK"; });
    return result;
}

bool mode01Payload(std::string_view reply, uint8_t pid, uint8_t count, uint32_t& result) {
    if (count == 0 || count > 4) return false;
    const auto shift = count * 8;
    const uint64_t expected = (0x41U << 8) | pid;
    const uint64_t mask = (uint64_t{1} << shift) - 1;
    bool found = false, conflict = false;
    uint32_t value = 0;
    forEachLine(reply, [&](std::string_view line) {
        uint64_t encoded = 0;
        if (!hexLine(line, count + 2, encoded) || encoded >> shift != expected) return;
        const auto payload = static_cast<uint32_t>(encoded & mask);
        if (found && payload != value) conflict = true;
        value = payload;
        found = true;
    });
    if (!found || conflict) return false;
    result = value;
    return true;
}

bool supported(uint32_t bitmap, uint8_t base, uint8_t pid) {
    const auto bit = pid - base;
    return bit > 0 && bit <= 32 && (bitmap & (uint32_t{1} << (32 - bit))) != 0;
}

bool updateMode01(Store& store, uint8_t pid, std::string_view reply, uint32_t now) {
    uint32_t raw = 0;
    if (!mode01Payload(reply, pid, pid == 0x0C ? 2 : 1, raw)) return false;
    switch (pid) {
        case 0x05: store.update(Signal::Coolant, static_cast<float>(raw) - 40, now); return true;
        case 0x0B: store.update(Signal::Map, static_cast<float>(raw), now); return raw > 0;
        case 0x0C: store.update(Signal::Rpm, static_cast<float>(raw) / 4, now); return true;
        case 0x0F: store.update(Signal::Intake, static_cast<float>(raw) - 40, now); return true;
        case 0x33: store.update(Signal::Barometer, static_cast<float>(raw), now); return raw > 0;
        default: return false;
    }
}

bool parseSerialFrame(std::string_view line, Frame& result) {
    if (line.substr(0, 3) != "H1,") return false;
    line.remove_prefix(3);
    Frame frame;
    for (size_t i = 0; i < DisplaySignals; ++i) {
        const auto comma = line.find(',');
        if ((i == DisplaySignals - 1) != (comma == std::string_view::npos)) return false;
        const auto field = line.substr(0, comma);
        if (!field.empty()) {
            float value;
            if (!number(field, value) || !readScalarsWithin(static_cast<Signal>(i), value)) return false;
            frame.values[i] = value;
            frame.available |= static_cast<uint8_t>(1U << i);
        }
        if (comma != std::string_view::npos) line.remove_prefix(comma + 1);
    }
    result = frame;
    return true;
}

size_t writeJson(const Frame& frame, bool demo, char* output, size_t size) {
    auto used = std::snprintf(output, size, "{\"version\":1,\"sequence\":%llu,%s\"values\":{",
                              static_cast<unsigned long long>(frame.sequence), demo ? "\"demo\":true," : "");
    if (used < 0 || static_cast<size_t>(used) >= size) return 0;
    bool first = true;
    for (size_t i = 0; i < DisplaySignals; ++i) {
        if ((frame.available & (1U << i)) == 0) continue;
        const auto added = std::snprintf(output + used, size - used, "%s\"%s\":%.2f", first ? "" : ",",
                                         DisplayKeys[i].data(), static_cast<double>(frame.values[i]));
        if (added < 0 || static_cast<size_t>(added) >= size - used) return 0;
        used += added;
        first = false;
    }
    if (size - used < 3) return 0;
    std::memcpy(output + used, "}}", 3);
    return static_cast<size_t>(used + 2);
}

} // namespace telemetry
