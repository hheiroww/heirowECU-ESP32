#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry {

constexpr uint32_t SignalLifetimeMs = 2500;
constexpr uint32_t BarometerLifetimeMs = 10000;
enum class Signal : uint8_t { Rpm, Boost, Intake, Coolant, Map, Barometer, Count };
constexpr size_t DisplaySignals = static_cast<size_t>(Signal::Map);

struct Frame {
    uint64_t sequence = 0;
    uint8_t available = 0;
    std::array<float, DisplaySignals> values{};
};

class Store {
public:
    void update(Signal signal, float value, uint32_t now);
    void replaceDisplay(const Frame& frame, uint32_t now);
    void clear();
    Frame snapshot(uint32_t now);

private:
    struct Reading { float value = 0; uint32_t at = 0; bool valid = false; };
    std::array<Reading, static_cast<size_t>(Signal::Count)> readings_{};
    uint64_t generation_ = 0;
    uint64_t publishedGeneration_ = 0;
    uint64_t sequence_ = 0;
    uint8_t lastAvailable_ = 0;
};

// ELM responses are complete prompt-terminated replies with headers disabled.
bool adapterOk(std::string_view reply);
bool mode01Payload(std::string_view reply, uint8_t pid, uint8_t count, uint32_t& result);
bool supported(uint32_t bitmap, uint8_t base, uint8_t pid);
bool updateMode01(Store& store, uint8_t pid, std::string_view reply, uint32_t now);

// H1,rpm,boost_psi,iat_c,coolant_c. An empty field means unavailable.
bool parseSerialFrame(std::string_view line, Frame& result);
size_t writeJson(const Frame& frame, bool demo, char* output, size_t size);

} // namespace telemetry
