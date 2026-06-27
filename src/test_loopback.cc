#include "modem_core.hh"
#include <iostream>

using namespace wasmmodem;

static bool roundtrip(WasmModem& m, const std::string& label, const std::string& msg) {

    const auto& samples = m.encode(msg);
    std::vector<float> stream;
    stream.insert(stream.end(), 4800, 0.0f);
    stream.insert(stream.end(), samples.begin(), samples.end());

    stream.insert(stream.end(), 60000, 0.0f);  // flush decoder pipeline
    auto frames = m.decode(stream.data(), stream.size());
    bool ok = !frames.empty() && frames.front().text == msg;
    std::cout << (ok ? "  OK   " : "  FAIL ") << label
              << "  payload=" << m.payloadSize()
              << " samples=" << samples.size()
              << " frames=" << frames.size();
    if (!frames.empty())
        std::cout << " snr=" << frames.front().snr;
    std::cout << "  rx=\"" << (frames.empty() ? "" : frames.front().text) << "\"\n";
    return ok;
}

int main(int argc, char** argv) {
    std::string msg = (argc > 1) ? argv[1] : "Hello from MODEM73 over WebAssembly! 123";
    auto m = std::make_unique<WasmModem>();
    int fails = 0;


    m->configure("WASM", 1500, "QPSK", "1/2", 1);
    fails += !roundtrip(*m, "OFDM QPSK 1/2 normal", msg);

    const char* names[] = {"MFSK-8", "MFSK-16", "MFSK-32", "MFSK-32R"};

    for (int mode = 0; mode < 4; mode++) {

        m->configureMfsk(1500, mode);



        std::string mm = msg.substr(0, std::min(msg.size(), (size_t)m->payloadSize()));
        fails += !roundtrip(*m, names[mode], mm);
    }


    std::cout << (fails ? "\nFAILURES: " : "\nALL OK (") << fails << (fails ? "\n" : ")\n");
    return fails ? 1 : 0;
}
