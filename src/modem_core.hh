#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <map>
#include <algorithm>

#include "modem.hh"            // Encoder48k, Decoder48k, ModemConfig
#include "phy/mfsk_modem.hh"   // MFSKEncoder, MFSKDecoder, MFSKMode, MFSKParams

namespace wasmmodem {


inline std::vector<uint8_t> frame_with_length(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> framed;
    uint16_t len = static_cast<uint16_t>(data.size());
    framed.push_back((len >> 8) & 0xFF);
    framed.push_back(len & 0xFF);
    framed.insert(framed.end(), data.begin(), data.end());
    return framed;
}

inline std::vector<uint8_t> unframe_length(const uint8_t* data, size_t total_len) {
    if (total_len < 2) return {};
    uint16_t payload_len = (data[0] << 8) | data[1];
    if (payload_len > total_len - 2)
        payload_len = static_cast<uint16_t>(total_len - 2);
    return std::vector<uint8_t>(data + 2, data + 2 + payload_len);
}

// --- Message fragmentation, byte-compatible with the native KISS TNC --------
// A message that fits one frame is sent raw (length-prefixed only). A larger
// message is split into 0xF3-headered fragments, each its own frame, exactly
// matching the native modem73 Fragmenter/Reassembler so the WASM build can
// exchange messages with a native TNC. These are single-threaded variants of
// the native classes (no mutex/atomic needed in the browser).
namespace Frag {
    constexpr uint8_t MAGIC = 0xF3;
    constexpr size_t  HEADER_SIZE = 5;
    constexpr uint8_t FLAG_MORE_FRAGMENTS = 0x01;
    constexpr uint8_t FLAG_FIRST_FRAGMENT = 0x02;
    constexpr size_t  MAX_PENDING_PACKETS = 64;
}

class Fragmenter {
public:
    // Split `data` into fragments whose total size (5-byte header + chunk) is
    // at most `max_payload`. Header layout: [0xF3][id_hi][id_lo][seq][flags].
    std::vector<std::vector<uint8_t>> fragment(const std::vector<uint8_t>& data,
                                               size_t max_payload) {
        std::vector<std::vector<uint8_t>> fragments;
        if (max_payload <= Frag::HEADER_SIZE) return fragments;

        size_t data_per_frag = max_payload - Frag::HEADER_SIZE;
        size_t num_frags = (data.size() + data_per_frag - 1) / data_per_frag;
        if (num_frags == 0) num_frags = 1;
        if (num_frags > 255) num_frags = 255;

        uint16_t packet_id = next_packet_id_++;
        for (size_t i = 0; i < num_frags; ++i) {
            size_t offset = i * data_per_frag;
            size_t chunk = std::min(data_per_frag, data.size() - offset);

            std::vector<uint8_t> frag;
            frag.reserve(Frag::HEADER_SIZE + chunk);
            frag.push_back(Frag::MAGIC);
            frag.push_back((packet_id >> 8) & 0xFF);
            frag.push_back(packet_id & 0xFF);
            frag.push_back(static_cast<uint8_t>(i));
            uint8_t flags = 0;
            if (i == 0) flags |= Frag::FLAG_FIRST_FRAGMENT;
            if (i < num_frags - 1) flags |= Frag::FLAG_MORE_FRAGMENTS;
            frag.push_back(flags);
            frag.insert(frag.end(), data.begin() + offset, data.begin() + offset + chunk);
            fragments.push_back(std::move(frag));
        }
        return fragments;
    }

    bool needs_fragmentation(size_t data_size, size_t max_payload) const {
        return data_size > (max_payload - Frag::HEADER_SIZE);
    }

private:
    uint16_t next_packet_id_ = 0;
};

class Reassembler {
public:
    bool is_fragment(const std::vector<uint8_t>& data) const {
        return data.size() >= Frag::HEADER_SIZE && data[0] == Frag::MAGIC;
    }

    // Feed one fragment; returns the full message once all fragments arrive,
    // otherwise an empty vector.
    std::vector<uint8_t> process(const std::vector<uint8_t>& fragment) {
        if (!is_fragment(fragment)) return {};

        uint16_t packet_id = (fragment[1] << 8) | fragment[2];
        uint8_t seq   = fragment[3];
        uint8_t flags = fragment[4];
        std::vector<uint8_t> payload(fragment.begin() + Frag::HEADER_SIZE, fragment.end());

        evict_if_needed();
        auto& pkt = pending_[packet_id];
        if (pkt.fragments.empty()) pkt.first_seen = ++tick_;
        pkt.fragments[seq] = std::move(payload);
        if (flags & Frag::FLAG_FIRST_FRAGMENT) pkt.has_first = true;
        if (!(flags & Frag::FLAG_MORE_FRAGMENTS)) { pkt.last_seq = seq; pkt.has_last = true; }

        if (pkt.has_first && pkt.has_last) {
            bool complete = true;
            for (uint8_t i = 0; i <= pkt.last_seq; ++i)
                if (pkt.fragments.find(i) == pkt.fragments.end()) { complete = false; break; }
            if (complete) {
                std::vector<uint8_t> out;
                for (uint8_t i = 0; i <= pkt.last_seq; ++i) {
                    auto& fd = pkt.fragments[i];
                    out.insert(out.end(), fd.begin(), fd.end());
                }
                pending_.erase(packet_id);
                return out;
            }
        }
        return {};
    }

    void reset() { pending_.clear(); }

private:
    struct PendingPacket {
        std::map<uint8_t, std::vector<uint8_t>> fragments;
        uint64_t first_seen = 0;
        uint8_t last_seq = 0;
        bool has_first = false;
        bool has_last = false;
    };

    // Drop the oldest in-progress packets if too many accumulate (lost tails).
    void evict_if_needed() {
        while (pending_.size() > Frag::MAX_PENDING_PACKETS) {
            auto oldest = pending_.begin();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (it->second.first_seen < oldest->second.first_seen) oldest = it;
            pending_.erase(oldest);
        }
    }

    std::map<uint16_t, PendingPacket> pending_;
    uint64_t tick_ = 0;
};

struct DecodedFrame {
    std::string text;
    double snr = 0.0;
    double ber = -1.0;   // per-frame bit error rate (FEC pre-correction), -1 if N/A
    int mod_bits = 0;    // bits per constellation symbol (OFDM only)
};

// Cumulative decode counters, mirrors the stats shown by the native TNC UI.
struct DecodeStats {
    int sync_count = 0;       // correlator preamble syncs
    int preamble_errors = 0;  // preamble decode failed
    int symbol_errors = 0;    // erasure budget exceeded (OFDM)
    int crc_errors = 0;       // polar CRC failed
    int erased_symbols = 0;   // symbols erased but frame continued (OFDM)
};




class WasmModem {
public:

    enum Type { OFDM = 0, MFSK = 1 };

    WasmModem() {
        encoder_ = std::make_unique<Encoder48k>();
        decoder_ = std::make_unique<Decoder48k>();
        mfsk_encoder_ = std::make_unique<MFSKEncoder>();
        mfsk_decoder_ = std::make_unique<MFSKDecoder>(mfsk_mode_, cfg_.center_freq);
        decoder_->configure_frontend(cfg_.center_freq, true);
    }

    int configure(const std::string& callsign, int center_freq,
                  const std::string& modulation, const std::string& code_rate,
                  int frame_size) {

        int64_t cs = ModemConfig::encode_callsign(callsign.c_str());
        if (cs < 0) return -1;
        int mode = ModemConfig::encode_mode(modulation.c_str(),
                                            code_rate.c_str(), frame_size);
        if (mode < 0) return -2;
        type_ = OFDM;
        cfg_.call_sign = cs;
        cfg_.center_freq = center_freq;
        cfg_.oper_mode = mode;
        decoder_->configure_frontend(cfg_.center_freq, true);
        return 0;
    }


    int configureMfsk(int center_freq, int mfsk_mode) {
        if (mfsk_mode < 0 || mfsk_mode > 3) return -3;
        type_ = MFSK;
        cfg_.center_freq = center_freq;
        mfsk_mode_ = static_cast<MFSKMode>(mfsk_mode);
        mfsk_decoder_->configure(mfsk_mode_, center_freq);
        return 0;
    }

    int activeType() const { return type_; }
    int centerFreq() const { return cfg_.center_freq; }


    int payloadSize() {
        int cap = (type_ == OFDM)
                      ? encoder_->get_payload_size(cfg_.oper_mode)
                      : mfsk_encoder_->get_payload_size(mfsk_mode_);
        return cap - 2;
    }




    // Inter-frame silence (samples @48k) between fragments so the decoder's
    // Schmidl-Cox preamble search re-locks on each frame.
    static constexpr size_t kInterFrameGap = 4800;

    const std::vector<float>& encode(const std::string& text) {
        std::vector<uint8_t> data(text.begin(), text.end());

        int cap = (type_ == OFDM) ? encoder_->get_payload_size(cfg_.oper_mode)
                                  : mfsk_encoder_->get_payload_size(mfsk_mode_);
        size_t max_payload = (cap > 2) ? static_cast<size_t>(cap - 2) : 0;

        // Match the native TNC: only fragment (0xF3 headers) when the message
        // exceeds one frame; otherwise send the raw bytes length-prefixed.
        std::vector<std::vector<uint8_t>> chunks;
        if (max_payload > Frag::HEADER_SIZE &&
            fragmenter_.needs_fragmentation(data.size(), max_payload)) {
            chunks = fragmenter_.fragment(data, max_payload);
        } else {
            chunks.push_back(std::move(data));
        }

        tx_samples_.clear();
        for (size_t c = 0; c < chunks.size(); ++c) {
            auto framed = frame_with_length(chunks[c]);
            std::vector<float> pcm;
            if (type_ == OFDM)
                pcm = encoder_->encode(framed.data(), framed.size(),
                                       cfg_.center_freq, cfg_.call_sign, cfg_.oper_mode);
            else
                pcm = mfsk_encoder_->encode(framed.data(), framed.size(),
                                            cfg_.center_freq, mfsk_mode_);
            if (c > 0) tx_samples_.insert(tx_samples_.end(), kInterFrameGap, 0.0f);
            tx_samples_.insert(tx_samples_.end(), pcm.begin(), pcm.end());
        }
        return tx_samples_;
    }




    std::vector<DecodedFrame> decode(const float* samples, size_t count) {
        std::vector<DecodedFrame> out;
        auto sink = [&](const uint8_t* d, size_t len) {
            auto payload = unframe_length(d, len);

            // Reassemble 0xF3 fragments; a non-fragment payload passes through
            // unchanged (backward compatible with single-frame messages).
            std::vector<uint8_t> msg;
            if (reassembler_.is_fragment(payload)) {
                msg = reassembler_.process(payload);
                if (msg.empty()) return;  // fragment stored; message incomplete
            } else {
                msg = std::move(payload);
            }

            DecodedFrame f;
            f.text.assign(msg.begin(), msg.end());
            if (type_ == OFDM) {
                f.snr = decoder_->get_last_snr();
                f.ber = decoder_->get_last_ber();
                f.mod_bits = decoder_->get_mod_bits();
            } else {
                f.snr = mfsk_decoder_->get_last_snr();
                f.ber = mfsk_decoder_->get_last_ber();
            }
            out.push_back(std::move(f));
        };
        if (type_ == OFDM)
            decoder_->process(samples, count, sink);
        else
            mfsk_decoder_->process(samples, count, sink);
        return out;
    }

    void reset() {
        if (type_ == OFDM) decoder_->reset();
        else mfsk_decoder_->reset();
        reassembler_.reset();
    }

    // Clear cumulative decode counters (and any half-reassembled messages).
    void reset_stats() {
        if (type_ == OFDM) {
            decoder_->stats_sync_count = 0;
            decoder_->stats_preamble_errors = 0;
            decoder_->stats_symbol_errors = 0;
            decoder_->stats_crc_errors = 0;
            decoder_->stats_erased_symbols = 0;
            decoder_->reset_ber();
        } else {
            mfsk_decoder_->stats_sync_count = 0;
            mfsk_decoder_->stats_preamble_errors = 0;
            mfsk_decoder_->stats_crc_errors = 0;
        }
        reassembler_.reset();
    }

    double last_snr() {
        return (type_ == OFDM) ? decoder_->get_last_snr()
                               : mfsk_decoder_->get_last_snr();
    }

    // Last per-frame BER (-1 if no frame decoded yet). Both modems track this.
    double last_ber() {
        return (type_ == OFDM) ? decoder_->get_last_ber()
                               : mfsk_decoder_->get_last_ber();
    }

    // Exponentially smoothed BER across frames (-1 until first frame).
    double ber_ema() {
        return (type_ == OFDM) ? decoder_->get_ber_ema()
                               : mfsk_decoder_->get_ber_ema();
    }

    // Cumulative decode counters (symbol/erasure fields are OFDM-only; the
    // MFSK decoder leaves them zero).
    DecodeStats stats() {
        DecodeStats s;
        if (type_ == OFDM) {
            s.sync_count      = decoder_->stats_sync_count;
            s.preamble_errors = decoder_->stats_preamble_errors;
            s.symbol_errors   = decoder_->stats_symbol_errors;
            s.crc_errors      = decoder_->stats_crc_errors;
            s.erased_symbols  = decoder_->stats_erased_symbols;
        } else {
            s.sync_count      = mfsk_decoder_->stats_sync_count;
            s.preamble_errors = mfsk_decoder_->stats_preamble_errors;
            s.crc_errors      = mfsk_decoder_->stats_crc_errors;
        }
        return s;
    }

private:
    Type type_ = OFDM;
    std::unique_ptr<Encoder48k> encoder_;
    std::unique_ptr<Decoder48k> decoder_;
    std::unique_ptr<MFSKEncoder> mfsk_encoder_;
    std::unique_ptr<MFSKDecoder> mfsk_decoder_;
    ModemConfig cfg_;
    MFSKMode mfsk_mode_ = MFSKMode::MFSK_16;
    std::vector<float> tx_samples_;
    Fragmenter fragmenter_;
    Reassembler reassembler_;
};

}  
