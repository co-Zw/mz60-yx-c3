#!/usr/bin/env python3
"""Exercise the real playback tasks and Opus decoder on the host.

The task fixtures supply fake codecs and RTOS calls; queue/lock/condition-variable
code is extracted from audio_service.cc. The decoder test compiles the real
wrapper against libopus, including FEC and its PLC fallback. No RF claims.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


TASK_PREFIX = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
template<class... Args> void discard_log(Args&&...) {}
#define ESP_LOGI(...) discard_log(__VA_ARGS__)
#define ESP_LOGW(...) discard_log(__VA_ARGS__)
#define ESP_LOGE(...) discard_log(__VA_ARGS__)
#define MAX_PLAYBACK_TASKS_IN_QUEUE 4
#define MAX_SEND_PACKETS_IN_QUEUE 4
#define PLAYBACK_PREFILL_TIMEOUT_MS 120
#define PLAYBACK_PREFILL_FRAMES 3
#define OPUS_FRAME_DURATION_MS 60
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000
static const char* TAG="AudioService";
constexpr int MALLOC_CAP_INTERNAL=0;
int heap_caps_get_free_size(int) { return 64000; }
void esp_timer_stop(int) {}
void esp_timer_start_periodic(int,int) {}
enum AudioTaskType { kAudioTaskTypeDecodeToPlaybackQueue,kAudioTaskTypeEncodeToSendQueue,kAudioTaskTypeEncodeToTestingQueue };
struct AudioStreamPacket {
    int sample_rate=24000,frame_duration=60;
    uint32_t timestamp=0;
    int64_t captured_at_us=0;
    std::vector<uint8_t> payload;
    bool decode_fec=false;
};
struct AudioTask {
    AudioTaskType type=kAudioTaskTypeDecodeToPlaybackQueue;
    std::vector<int16_t> pcm;
    uint32_t timestamp=0;
    int64_t captured_at_us=0;
};
struct FakeDecoder {
    std::function<bool(bool)> hook;
    int sample_rate() { return 24000; }
    bool Decode(std::vector<uint8_t>&&,std::vector<int16_t>& pcm,bool fec) {
        pcm.resize(1440); return hook(fec);
    }
};
struct FakeEncoder { bool Encode(std::vector<int16_t>&&,std::vector<uint8_t>&) { return false; } };
struct FakeResampler {
    int GetOutputSamples(int samples) { return samples; }
    void Process(const int16_t*,int,int16_t*) {}
};
struct FakeCodec {
    std::function<void()> write;
    bool output_enabled() { return true; }
    void EnableOutput(bool) {}
    int output_sample_rate() { return 24000; }
    void OutputData(std::vector<int16_t>&) { write(); }
};
struct AudioService {
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_,audio_send_queue_,audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_,audio_encode_queue_;
    bool service_stopped_=false,playback_buffering_=true,playback_active_=false,decode_active_=false;
    uint32_t playback_generation_=0;
    std::atomic<uint32_t> input_generation_{0};
    std::unique_ptr<FakeDecoder> opus_decoder_=std::make_unique<FakeDecoder>();
    std::unique_ptr<FakeEncoder> opus_encoder_=std::make_unique<FakeEncoder>();
    FakeResampler output_resampler_;
    FakeCodec* codec_;
    int audio_power_timer_=0;
    const void* curr_play_sound_=nullptr;
    std::chrono::steady_clock::time_point last_output_time_;
    struct { int playback_count=0,decode_count=0,encode_count=0; } debug_statistics_;
    struct { std::function<void()> on_playback_finished; } callbacks_;
    void SetDecodeSampleRate(int,int) {}
    void QueuePacketForSend(std::unique_ptr<AudioStreamPacket>,uint32_t) {}
    bool IsPlaybackCompleteLocked() const;
    bool WaitForPlayCompletion(int);
    bool IsIdle();
    void OpusCodecTask();
    void AudioOutputTask();
};
enum DeviceState { kDeviceStateIdle,kDeviceStateSpeaking,kDeviceStateListening };
enum ListeningMode { kListeningModeAutoStop,kListeningModeManualStop };
struct Application {
    AudioService& audio_service_;
    bool tts_playback_pending_=true,chat_enabled_=true,aborted_=false;
    DeviceState device_state_=kDeviceStateSpeaking;
    ListeningMode listening_mode_=kListeningModeAutoStop;
    void SetDeviceState(DeviceState state) { device_state_=state; }
    void FinishSpeakingIfPlaybackComplete();
};
'''

TASK_TEST = r'''
int main() {
    AudioService service; FakeCodec codec; service.codec_=&codec;
    Application app{service};
    std::mutex barrier; std::condition_variable gate;
    bool decoding=false,allow_decode=false,writing=false,allow_write=false;
    std::atomic<int> finished{0},written{0};
    service.callbacks_.on_playback_finished=[&] { ++finished; };
    service.opus_decoder_->hook=[&](bool fec) {
        std::unique_lock<std::mutex> lock(barrier);
        if(!decoding) { assert(fec); decoding=true; gate.notify_all(); gate.wait(lock,[&]{return allow_decode;}); }
        return true;
    };
    codec.write=[&] {
        std::unique_lock<std::mutex> lock(barrier); writing=true; gate.notify_all();
        gate.wait(lock,[&]{return allow_write;}); ++written;
    };
    auto enqueue=[&](bool fec) {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        auto packet=std::make_unique<AudioStreamPacket>(); packet->decode_fec=fec;
        service.audio_decode_queue_.push_back(std::move(packet)); service.audio_queue_cv_.notify_all();
    };
    auto wait_for=[&](bool& flag) {
        std::unique_lock<std::mutex> lock(barrier);
        assert(gate.wait_for(lock,std::chrono::seconds(3),[&]{return flag;}));
    };
    enqueue(true);
    std::thread decoder([&]{service.OpusCodecTask();}); wait_for(decoding);
    // The packet is no longer in either queue, but has not finished decoding.
    assert(!service.WaitForPlayCompletion(0) && !service.IsIdle());
    app.FinishSpeakingIfPlaybackComplete(); assert(app.device_state_==kDeviceStateSpeaking);
    { std::lock_guard<std::mutex> lock(barrier); allow_decode=true; } gate.notify_all();
    std::thread output([&]{service.AudioOutputTask();}); wait_for(writing);
    for(int i=0;i<24;++i) enqueue(false); // 1.5 s of audio, beyond the old timeout
    const auto start=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(1100)) {
        app.FinishSpeakingIfPlaybackComplete();
        assert(app.device_state_==kDeviceStateSpeaking && app.tts_playback_pending_ && !finished);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    { std::lock_guard<std::mutex> lock(barrier); allow_write=true; } gate.notify_all();
    assert(service.WaitForPlayCompletion(3000));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!finished && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(finished && written==25);
    app.FinishSpeakingIfPlaybackComplete();
    assert(app.device_state_==kDeviceStateListening && !app.tts_playback_pending_);
    // A cancelled/disabled/new TTS turn cannot be completed by an old event.
    app.device_state_=kDeviceStateSpeaking; app.tts_playback_pending_=false;
    app.FinishSpeakingIfPlaybackComplete(); assert(app.device_state_==kDeviceStateSpeaking);
    app.tts_playback_pending_=true; app.chat_enabled_=false;
    app.FinishSpeakingIfPlaybackComplete(); assert(app.device_state_==kDeviceStateSpeaking);
    app.chat_enabled_=true; app.aborted_=true;
    app.FinishSpeakingIfPlaybackComplete(); assert(app.device_state_==kDeviceStateSpeaking);
    app.aborted_=false; app.listening_mode_=kListeningModeManualStop;
    app.FinishSpeakingIfPlaybackComplete(); assert(app.device_state_==kDeviceStateIdle);
    { std::lock_guard<std::mutex> lock(service.audio_queue_mutex_); service.service_stopped_=true; service.audio_queue_cv_.notify_all(); }
    decoder.join(); output.join();
    puts("PASS: in-flight decode/output prevent premature completion; 1.5 s tail drains without blocking main task; manual stop and cancellation guards");
}
'''

OPUS_TEST = r'''
#include "opus_decoder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
using Bytes=std::vector<uint8_t>;
int main() {
    int error; auto* encoder=opus_encoder_create(24000,1,OPUS_APPLICATION_VOIP,&error);
    assert(encoder && !error);
    assert(!opus_encoder_ctl(encoder,OPUS_SET_BITRATE(16000)));
    assert(!opus_encoder_ctl(encoder,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)));
    assert(!opus_encoder_ctl(encoder,OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)));
    assert(!opus_encoder_ctl(encoder,OPUS_SET_INBAND_FEC(1)));
    assert(!opus_encoder_ctl(encoder,OPUS_SET_PACKET_LOSS_PERC(30)));
    std::vector<Bytes> packets;
    for(int p=0;p<50;++p) {
        std::vector<opus_int16> pcm(1440);
        for(int i=0;i<1440;++i) {
            double t=(p*1440+i)/24000.0;
            pcm[i]=(std::sin(t*2*3.141592653589793*140)*0.7+std::sin(t*2*3.141592653589793*290)*0.3)*9000;
        }
        Bytes packet(1275); int bytes=opus_encode(encoder,pcm.data(),pcm.size(),packet.data(),packet.size());
        assert(bytes>0); packet.resize(bytes); packets.push_back(std::move(packet));
    }
    OpusDecoderWrapper wrapper(24000,1,60),plc(24000,1,60);
    auto* reference=opus_decoder_create(24000,1,&error); assert(reference && !error);
    std::vector<int16_t> actual,estimated; bool recovered_different=false;
    auto check=[&](Bytes payload,bool fec) {
        std::vector<int16_t> expected(1440);
        int samples=opus_decode(reference,payload.empty()?nullptr:payload.data(),payload.size(),expected.data(),1440,fec);
        assert(samples==1440); expected.resize(samples);
        assert(wrapper.Decode(Bytes(payload),actual,fec) && actual==expected);
    };
    for(size_t p=0;p<packets.size();++p) {
        if(p>0 && p%10==0) {
            ++p; check(packets[p],true);
            assert(plc.Decode({},estimated) && estimated.size()==1440);
            recovered_different|=actual!=estimated;
        }
        check(packets[p],false); assert(plc.Decode(Bytes(packets[p]),estimated));
    }
    assert(recovered_different); // actual encoder redundancy differs from plain PLC
    // A CELT-only packet has no SILK FEC: the wrapper must equal plain PLC.
    opus_encoder_destroy(encoder); encoder=opus_encoder_create(24000,1,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&error);
    assert(encoder && !error);
    wrapper.ResetState(); opus_decoder_ctl(reference,OPUS_RESET_STATE);
    std::vector<int16_t> silence(1440); Bytes no_fec(1275);
    int bytes=opus_encode(encoder,silence.data(),1440,no_fec.data(),no_fec.size()); assert(bytes>0); no_fec.resize(bytes);
    check(no_fec,false);
    std::vector<int16_t> expected(1440);
    assert(opus_decode(reference,nullptr,0,expected.data(),1440,0)==1440);
    assert(wrapper.Decode(Bytes(no_fec),actual,true) && actual==expected);
    check(no_fec,false);
    // Invalid redundancy still emits a bounded PLC frame.
    assert(opus_decode(reference,nullptr,0,expected.data(),1440,0)==1440);
    assert(wrapper.Decode({0xff},actual,true) && actual==expected);
    opus_encoder_destroy(encoder); opus_decoder_destroy(reference);
    puts("PASS: real Opus FEC and following frame match raw decoder; absent/corrupt redundancy safely falls back to 60 ms PLC");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="audio-playback-") as directory:
        output = Path(directory)
        (output / "esp_log.h").write_text("#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n")
        methods = "".join(function("main/audio/audio_service.cc", signature) for signature in (
            "void AudioService::AudioOutputTask(", "void AudioService::OpusCodecTask(",
            "bool AudioService::IsPlaybackCompleteLocked(", "bool AudioService::WaitForPlayCompletion(",
            "bool AudioService::IsIdle(",
        ))
        methods += function("main/application.cc", "void Application::FinishSpeakingIfPlaybackComplete(")
        task_source = output / "tasks.cc"
        task_source.write_text(TASK_PREFIX + methods + TASK_TEST)
        opus_source = output / "opus.cc"
        opus_source.write_text(OPUS_TEST)
        for name, sources, extra in (
            ("tasks", [task_source], []),
            ("opus", [opus_source, ROOT / "components/78__esp-opus-encoder/opus_decoder.cc"], ["-l:libopus.so.0"]),
        ):
            executable = output / name
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                "-I", str(output), "-I", str(ROOT / "components/78__esp-opus-encoder/include"),
                "-I", str(ROOT / "managed_components/78__esp-opus/include"),
                *map(str, sources), "-o", str(executable), *extra,
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
