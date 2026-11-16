#include "opus_decoder.h"
#include <esp_log.h>

#define TAG "OpusDecoderWrapper"

OpusDecoderWrapper::OpusDecoderWrapper(int sample_rate, int channels, int duration_ms)
    : sample_rate_(sample_rate), duration_ms_(duration_ms), channels_(channels) {
    int error;
    audio_dec_ = opus_decoder_create(sample_rate, channels, &error);
    if (audio_dec_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", error);
        return;
    }

    frame_size_ = sample_rate / 1000 * channels * duration_ms;
}

OpusDecoderWrapper::~OpusDecoderWrapper() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_dec_ != nullptr) {
        opus_decoder_destroy(audio_dec_);
    }
}

bool OpusDecoderWrapper::Decode(std::vector<uint8_t>&& opus, std::vector<int16_t>& pcm, bool decode_fec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_dec_ == nullptr) {
        ESP_LOGE(TAG, "Audio decoder is not configured");
        return false;
    }

    pcm.resize(frame_size_);
    if (decode_fec) {
        const bool has_fec = !opus.empty() && opus_packet_has_lbrr(opus.data(), opus.size()) > 0;
        auto& count = has_fec ? fec_packet_count_ : plc_packet_count_;
        ++count;
        if (count == 1 || (fec_packet_count_ + plc_packet_count_) % 16 == 0) {
            ESP_LOGI(TAG, "Opus loss recovery: FEC packets=%u, PLC fallback=%u",
                     fec_packet_count_, plc_packet_count_);
        }
    }
    // FEC reads redundancy for the preceding lost frame, without consuming the
    // following frame. Opus falls back to PLC when the packet carries no FEC.
    auto ret = opus_decode(audio_dec_, opus.empty() ? nullptr : opus.data(), opus.size(),
                           pcm.data(), frame_size_ / channels_, decode_fec ? 1 : 0);
    if (ret < 0 && decode_fec) {
        ret = opus_decode(audio_dec_, nullptr, 0, pcm.data(), frame_size_ / channels_, 0);
    }
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to decode audio, error code: %d", ret);
        return false;
    }

    pcm.resize(ret * channels_);
    return true;
}

void OpusDecoderWrapper::ResetState() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_dec_ != nullptr) {
        opus_decoder_ctl(audio_dec_, OPUS_RESET_STATE);
    }
}

void OpusDecoderWrapper::Config(int sample_rate, int channels, int duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_dec_ == nullptr) {
        ESP_LOGE(TAG, "Audio decoder is not create");
        return;
    }

    // Decoder storage is allocated for the constructor's channel count.
    if (channels != channels_ || opus_decoder_init(audio_dec_, sample_rate, channels) != OPUS_OK) {
        ESP_LOGE(TAG, "Invalid audio decoder configuration");
        return;
    }

    sample_rate_ = sample_rate;
    duration_ms_ = duration_ms;

    frame_size_ = sample_rate / 1000 * channels * duration_ms;
}
