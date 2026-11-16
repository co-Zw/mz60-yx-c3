#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <opus.h>

// Merge the external codec's short frames into the 60 ms packets advertised
// to the server. Opus repacketizing preserves the encoded audio without another
// encode/decode pass. Only the audio input task owns this state.
class OpusPacketBatcher {
public:
    struct Packet {
        std::vector<uint8_t> payload;
        int duration_ms;
        int64_t captured_at_us;
    };

    OpusPacketBatcher()
        : state_(opus_repacketizer_create(), opus_repacketizer_destroy) {}

    void Reset() {
        if (state_) {
            opus_repacketizer_init(state_.get());
        }
        inputs_.clear();
        samples_ = 0;
        payload_bytes_ = 0;
    }

    int64_t OldestCaptureTime() const {
        return inputs_.empty() ? 0 : inputs_.front().captured_at_us;
    }

    template<typename Emit>
    bool Add(std::vector<uint8_t> payload, int64_t captured_at_us, Emit&& emit) {
        if (payload.empty()) {
            return false;
        }
        const int samples = opus_packet_get_nb_samples(payload.data(), payload.size(), 16000);
        if (samples <= 0 || samples > 1920) {
            return false;
        }
        if (!state_ || samples > kTargetSamples) {
            Flush(emit);
            emit(Packet{std::move(payload), (samples + 15) / 16, captured_at_us});
            return true;
        }
        if (samples_ + samples > kTargetSamples) {
            Flush(emit);
        }
        if (opus_repacketizer_cat(state_.get(), payload.data(), payload.size()) != OPUS_OK) {
            // A codec mode/bandwidth/frame-size change cannot be merged with
            // the preceding frames. Deliver those first, then start a new batch.
            Flush(emit);
            if (opus_repacketizer_cat(state_.get(), payload.data(), payload.size()) != OPUS_OK) {
                return false;
            }
        }
        payload_bytes_ += payload.size();
        samples_ += samples;
        // libopus retains pointers into each input until repacketizing finishes.
        inputs_.push_back({std::move(payload), (samples + 15) / 16, captured_at_us});
        if (samples_ == kTargetSamples) {
            Flush(emit);
        }
        return true;
    }

    template<typename Emit>
    void Flush(Emit&& emit) {
        if (inputs_.empty()) {
            return;
        }
        Packet packet{{}, (samples_ + 15) / 16, inputs_.front().captured_at_us};
        // Space for the input bytes plus all possible per-frame length fields.
        packet.payload.resize(payload_bytes_ + 2 * opus_repacketizer_get_nb_frames(state_.get()) + 2);
        const int bytes = opus_repacketizer_out(state_.get(), packet.payload.data(), packet.payload.size());
        if (bytes > 0) {
            packet.payload.resize(bytes);
            Reset();
            emit(std::move(packet));
        } else {
            auto originals = std::move(inputs_);
            Reset();
            for (auto& original : originals) {
                emit(std::move(original));
            }
        }
    }

private:
    static constexpr int kTargetSamples = 60 * 16;
    std::unique_ptr<OpusRepacketizer, decltype(&opus_repacketizer_destroy)> state_;
    std::vector<Packet> inputs_;
    int samples_ = 0;
    size_t payload_bytes_ = 0;
};
