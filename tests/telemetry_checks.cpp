#include "Telemetry.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

using telemetry::Frame;
using telemetry::Signal;
using telemetry::Store;

namespace {
constexpr auto bit(Signal signal) { return 1U << static_cast<unsigned>(signal); }
constexpr auto indexOf(Signal signal) { return static_cast<size_t>(signal); }

float get(const Frame& frame, Signal signal) {
    assert((frame.available & bit(signal)) != 0);
    return frame.values[indexOf(signal)];
}
} // namespace

int main() {
    assert(telemetry::adapterOk("ATE0\rOK\r"));
    assert(!telemetry::adapterOk("CHECK OK\r?\r"));
    uint32_t payload = 0;
    assert(telemetry::mode01Payload("0100\rSEARCHING...\r41 00 BE 3F A8 13\r", 0, 4, payload));
    assert(payload == 0xBE3FA813);
    assert(telemetry::supported(payload, 0, 0x0C));
    assert(telemetry::supported(payload, 0, 0x20));
    assert(telemetry::supported(0x00002000, 0x20, 0x33));
    assert(!telemetry::supported(0xFFFFFFFF, 0x20, 0x1F));
    assert(!telemetry::supported(0xFFFFFFFF, 0x20, 0x41));

    assert(telemetry::mode01Payload("410C2C88\r\n41 0c 2c 88\r", 0x0c, 2, payload) && payload == 11400);
    assert(!telemetry::mode01Payload("41 0C 2C 88\r41 0C 34 56\r", 0x0C, 2, payload));
    assert(!telemetry::mode01Payload("NO DATA\rSTOPPED\r", 0x0C, 2, payload));
    assert(!telemetry::mode01Payload("7E8 04 41 0C 2C 88", 0x0C, 2, payload));
    assert(!telemetry::mode01Payload("41 05 5C\r7F 01 11", 0x0C, 2, payload));
    assert(!telemetry::mode01Payload("41 0C 2C", 0x0C, 2, payload));

    Store store;
    assert(store.snapshot(1000).available == 0);
    assert(telemetry::updateMode01(store, 0x0C, "41 0C 2C 88", 1000));
    assert(telemetry::updateMode01(store, 0x0B, "41 0B 8A", 1000));
    assert(telemetry::updateMode01(store, 0x0F, "41 0F 4A", 1000));
    assert(telemetry::updateMode01(store, 0x05, "41 05 83", 1000));
    auto partial = store.snapshot(1000);
    assert(partial.available == (bit(Signal::Rpm) | bit(Signal::Intake) | bit(Signal::Coolant)));
    assert(get(partial, Signal::Rpm) == 2850);
    assert(get(partial, Signal::Intake) == 34);
    assert(get(partial, Signal::Coolant) == 91);
    assert(telemetry::updateMode01(store, 0x33, "41 33 64", 1010));
    const auto full = store.snapshot(1010);
    assert(full.available == 15);
    assert(std::abs(get(full, Signal::Boost) - 5.511434f) < 0.0001f);
    assert(full.sequence > partial.sequence);
    assert(store.snapshot(1050).sequence == full.sequence);
    assert(store.snapshot(3499).available == 15);
    assert(store.snapshot(3500).available == 0);
    assert(telemetry::updateMode01(store, 0x0B, "41 0B 3C", 11008));
    assert(get(store.snapshot(11009), Signal::Boost) < 0);
    assert(store.snapshot(11010).available == 0);
    assert(!telemetry::updateMode01(store, 0x0B, "41 0B 00", 11011));
    store.update(Signal::Rpm, 1000, 0xfffffff0U);
    assert(store.snapshot(0x12U).available == bit(Signal::Rpm));
    assert(store.snapshot(0x1000U).available == 0);

    Frame direct;
    assert(telemetry::parseSerialFrame("H1,2850,9.4,34,91", direct) && direct.available == 15);
    store.replaceDisplay(direct, 16000);
    auto displayed = store.snapshot(16000);
    assert(get(displayed, Signal::Boost) == 9.4f);
    unsigned sequence = static_cast<unsigned>(displayed.sequence);
    char output[192]{};
    assert(telemetry::writeJson(displayed, true, output, sizeof(output)) > 0);
    assert(std::string_view(output).find("\"demo\":true") != std::string_view::npos);
    assert(std::string_view(output).find("\"boost_psi\":9.40") != std::string_view::npos);
    assert(telemetry::parseSerialFrame("H1,2950,,33,", direct) && direct.available == 5);
    store.replaceDisplay(direct, 16010);
    displayed = store.snapshot(16011);
    assert(displayed.available == 5 && displayed.sequence > sequence);
    assert(telemetry::writeJson(displayed, false, output, sizeof(output)) > 0);
    assert(std::string_view(output).find("boost_psi") == std::string_view::npos);
    assert(std::string_view(output).find("coolant_c") == std::string_view::npos);
    assert(std::string_view(output).find("demo") == std::string_view::npos);
    assert(telemetry::writeJson(displayed, false, output, 40) == 0);
    assert(telemetry::parseSerialFrame("H1,,,,", direct) && direct.available == 0);
    assert(!telemetry::parseSerialFrame("H1,2850,inf,34,91", direct));
    assert(!telemetry::parseSerialFrame("H1,2850,9.4,34,91,0", direct));
    assert(!telemetry::parseSerialFrame("H1,2850,9.4,34", direct));
    assert(!telemetry::parseSerialFrame("H1,2850,9.4,34,910", direct));
    assert(!telemetry::parseSerialFrame("H1,2850,0x5,34,91", direct));
    std::cout << "PASS ELM replies, channel decoding, serial frames, rollover, expiry, and JSON\n";
}
