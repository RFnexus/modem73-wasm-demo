// Embind bindings exposing the MODEM73 OFDM modem core to JavaScript.
//
//   const m = new Module.Modem();
//   m.configure("WASM", 1500, "QPSK", "1/2", 1);   // 0 on success
//   const pcm = m.encode("hello");                  // Float32Array (48 kHz mono)
//   const frames = m.decode(pcm);                   // [{text, snr}, ...]
//
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "modem_core.hh"

using namespace emscripten;
using wasmmodem::WasmModem;
using wasmmodem::DecodeStats;

namespace {

// Encode and hand JS a *copy* as a fresh Float32Array (the core's buffer is
// reused on the next encode call, so we must not expose a view into it).
val encode_to_f32(WasmModem& self, const std::string& text) {
    const std::vector<float>& s = self.encode(text);
    val out = val::global("Float32Array").new_(s.size());
    out.call<void>("set", val(typed_memory_view(s.size(), s.data())));
    return out;
}

// Accept a JS Float32Array (or any numeric array) and stream it through the
// decoder. Returns a JS array of {text, snr} objects.
val decode_from_js(WasmModem& self, val pcm) {
    std::vector<float> samples = convertJSArrayToNumberVector<float>(pcm);
    auto frames = self.decode(samples.data(), samples.size());
    val arr = val::array();
    for (size_t i = 0; i < frames.size(); ++i) {
        val o = val::object();
        o.set("text", frames[i].text);
        o.set("snr", frames[i].snr);
        o.set("ber", frames[i].ber);
        o.set("modBits", frames[i].mod_bits);
        arr.set(i, o);
    }
    return arr;
}

// Cumulative decode counters as a plain JS object (mirrors the native TNC UI).
val stats_to_js(WasmModem& self) {
    DecodeStats s = self.stats();
    val o = val::object();
    o.set("syncCount", s.sync_count);
    o.set("preambleErrors", s.preamble_errors);
    o.set("symbolErrors", s.symbol_errors);
    o.set("crcErrors", s.crc_errors);
    o.set("erasedSymbols", s.erased_symbols);
    return o;
}

}  // namespace

EMSCRIPTEN_BINDINGS(modem73) {
    class_<WasmModem>("Modem")
        .constructor<>()
        .function("configure", &WasmModem::configure)        // OFDM
        .function("configureMfsk", &WasmModem::configureMfsk) // MFSK
        .function("activeType", &WasmModem::activeType)       // 0=OFDM 1=MFSK
        .function("payloadSize", &WasmModem::payloadSize)
        .function("centerFreq", &WasmModem::centerFreq)
        .function("encode", &encode_to_f32)
        .function("decode", &decode_from_js)
        .function("reset", &WasmModem::reset)
        .function("lastSnr", &WasmModem::last_snr)
        .function("lastBer", &WasmModem::last_ber)
        .function("berEma", &WasmModem::ber_ema)
        .function("stats", &stats_to_js)
        .function("resetStats", &WasmModem::reset_stats);
}
