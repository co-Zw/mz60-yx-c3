#!/usr/bin/env python3
"""Native checks for the real Opus batcher and UDP send implementation.

Requires g++ and libopus.so. The UDP transport is fault-injected; these checks
cannot establish the reliability of a physical Wi-Fi link.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

BATCH_TEST = r'''
#include "main/audio/opus_packet_batcher.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
using Bytes=std::vector<uint8_t>;
Bytes encode(OpusEncoder* encoder,int samples,int offset) {
    std::vector<opus_int16> pcm(samples);
    for(int i=0;i<samples;++i) pcm[i]=std::sin((i+offset)*0.079)*3000;
    Bytes packet(1275);
    int size=opus_encode(encoder,pcm.data(),samples,packet.data(),packet.size());
    assert(size>0); packet.resize(size); return packet;
}
OpusEncoder* encoder(int application=OPUS_APPLICATION_VOIP) {
    int error;
    auto* result=opus_encoder_create(16000,1,application,&error);
    assert(result && error==OPUS_OK);
    assert(opus_encoder_ctl(result,OPUS_SET_BITRATE(16000))==OPUS_OK);
    assert(opus_encoder_ctl(result,OPUS_SET_VBR(0))==OPUS_OK);
    assert(opus_encoder_ctl(result,OPUS_SET_DTX(0))==OPUS_OK);
    return result;
}
std::vector<opus_int16> decode(const std::vector<Bytes>& packets) {
    int error; auto* decoder=opus_decoder_create(16000,1,&error);
    assert(decoder && error==OPUS_OK);
    std::vector<opus_int16> result;
    for(const auto& packet:packets) {
        opus_int16 pcm[1920];
        int size=opus_decode(decoder,packet.data(),packet.size(),pcm,1920,0);
        assert(size>0); result.insert(result.end(),pcm,pcm+size);
    }
    opus_decoder_destroy(decoder); return result;
}
int main() {
    auto* enc=encoder();
    std::vector<Bytes> originals;
    for(int i=0;i<6;++i) originals.push_back(encode(enc,320,i*320));
    for(const auto& data:originals) assert(opus_packet_get_nb_samples(data.data(),data.size(),16000)==320);
    OpusPacketBatcher batcher;
    std::vector<OpusPacketBatcher::Packet> output;
    auto emit=[&](OpusPacketBatcher::Packet packet) { output.push_back(std::move(packet)); };
    for(int i=0;i<6;++i) {
        auto temporary=originals[i];
        assert(batcher.Add(std::move(temporary),100000+i*20000,emit));
        assert(output.size()==static_cast<size_t>((i+1)/3));
    }
    assert(output.size()==2 && output[0].duration_ms==60 && output[1].duration_ms==60);
    assert(output[0].captured_at_us==100000 && output[1].captured_at_us==160000);
    assert(decode(originals)==decode({output[0].payload,output[1].payload}));
    // A partial batch must not leak across a new listening/session generation.
    output.clear(); assert(batcher.Add(originals[0],1,emit)); batcher.Reset();
    assert(!batcher.OldestCaptureTime());
    for(int i=0;i<3;++i) assert(batcher.Add(originals[i],200000+i*20000,emit));
    assert(output.size()==1 && output[0].captured_at_us==200000);
    assert(decode({originals[0],originals[1],originals[2]})==decode({output[0].payload}));
    // DTX/input timeouts can flush a short tail without fabricating silence.
    output.clear(); assert(batcher.Add(originals[0],300000,emit));
    assert(batcher.OldestCaptureTime()==300000); batcher.Flush(emit);
    assert(output.size()==1 && output[0].duration_ms==20 && !batcher.OldestCaptureTime());
    // Invalid packets cannot destroy the valid audio already waiting.
    output.clear(); assert(batcher.Add(originals[0],400000,emit));
    assert(!batcher.Add({},420000,emit)); assert(!batcher.Add({0xff},420000,emit));
    batcher.Flush(emit); assert(output.size()==1 && output[0].duration_ms==20);
    // Configuration/frame-size changes flush old frames before the new format.
    auto* short_enc=encoder(OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    output.clear(); assert(batcher.Add(originals[0],500000,emit));
    assert(batcher.Add(originals[1],520000,emit));
    for(int i=0;i<6;++i) assert(batcher.Add(encode(short_enc,160,i*160),540000+i*10000,emit));
    assert(output.size()==2 && output[0].duration_ms==40 && output[1].duration_ms==60);
    assert(decode({output[0].payload}).size()==640 && decode({output[1].payload}).size()==960);
    opus_encoder_destroy(enc); opus_encoder_destroy(short_enc);
    puts("PASS: real Opus 20-to-60 ms batching preserves decoded speech exactly; input lifetime, generation reset, short tails, malformed packets and codec changes");
}
'''

UDP_PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
template<class... Args> void discard_log(Args&&...) {}
#define ESP_LOGI(...) discard_log(__VA_ARGS__)
#define ESP_LOGW(...) discard_log(__VA_ARGS__)
#define ESP_LOGE(...) discard_log(__VA_ARGS__)
#define MSG_DONTWAIT 1
#define pdMS_TO_TICKS(x) (x)
static const char* TAG="EspUdp";
static int64_t now_us=1000000;
static int calls=0;
static std::function<int(size_t)> send_hook;
static std::function<int(char*,size_t)> recv_hook;
static std::function<void()> delay_hook;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(int ms) { assert(ms>0 && ms<=40); now_us+=ms*1000; if(delay_hook) delay_hook(); }
int send(int,const char*,size_t bytes,int flags) { assert(flags==MSG_DONTWAIT); ++calls; return send_hook(bytes); }
int recv(int,char* data,size_t size,int) { return recv_hook(data,size); }
struct EspUdp {
    int udp_fd_=1;
    std::atomic<bool> connected_{true};
    int64_t tx_congestion_since_us_=0,tx_stable_since_us_=0,tx_last_success_us_=0;
    int64_t tx_last_send_us_=0;
    unsigned tx_dropped_packets_=0,tx_delivered_packets_=0;
    std::function<void()> disconnected_callback_;
    std::function<void()> receive_idle_callback_;
    std::function<void(const std::string&)> message_callback_;
    int Send(const std::string&);
    void ReceiveTask();
};
'''

UDP_TEST = r'''
int main() {
    const std::string packet(120,'a');
    auto busy=[](size_t) { errno=ENOMEM; return -1; };
    int disconnected=0;
    {
        EspUdp udp; auto started=now_us;
        send_hook=[&](size_t bytes) { return now_us-started<180000 ? busy(bytes) : int(bytes); };
        assert(udp.Send(packet)==120 && now_us-started==190000 && !udp.tx_dropped_packets_);
        // A single success after retries must not clear congestion history.
        assert(udp.tx_congestion_since_us_ && udp.tx_delivered_packets_==1);
        send_hook=[](size_t bytes) { return int(bytes); };
        now_us+=500000; assert(udp.Send(packet)==120 && udp.tx_congestion_since_us_);
        now_us+=500000; assert(udp.Send(packet)==120 && !udp.tx_congestion_since_us_);
    }
    {
        EspUdp udp; udp.disconnected_callback_=[&] { ++disconnected; };
        send_hook=busy; auto started=now_us;
        assert(udp.Send(packet)==0 && now_us-started==200000 && udp.tx_dropped_packets_==1);
        while(udp.connected_) udp.Send(packet);
        assert(disconnected==1 && now_us-started>=2000000 && now_us-started<=2200000);
        int old_calls=calls; assert(udp.Send(packet)==-1 && calls==old_calls && disconnected==1);
    }
    {
        // The reported failure alternates failed speech with occasional success.
        // Those successes must not indefinitely conceal a largely unusable uplink.
        EspUdp udp; udp.disconnected_callback_=[&] { ++disconnected; };
        auto started=now_us;
        for(int i=0;udp.connected_ && i<30;++i) {
            send_hook=busy; udp.Send(packet);
            if(udp.connected_) {
                send_hook=[](size_t bytes) { return int(bytes); };
                now_us+=40000; assert(udp.Send(packet)==120);
                assert(udp.tx_congestion_since_us_ && udp.tx_dropped_packets_);
            }
        }
        assert(!udp.connected_ && disconnected==2 && now_us-started<4000000);
    }
    {
        EspUdp udp; udp.disconnected_callback_=[&] { ++disconnected; };
        send_hook=[](size_t) { errno=ECONNRESET; return -1; };
        auto started=now_us; assert(udp.Send(packet)==-1 && now_us==started && disconnected==3);
    }
    {
        // A long TTS reply is not a network stall. Resume with a fresh budget,
        // even if the previous listening turn had not reached stable recovery.
        EspUdp udp; send_hook=busy;
        assert(udp.Send(packet)==0);
        send_hook=[](size_t bytes) { return int(bytes); };
        assert(udp.Send(packet)==120 && udp.tx_congestion_since_us_);
        now_us+=5000000; send_hook=busy;
        auto started=now_us; assert(udp.Send(packet)==0 && udp.connected_);
        assert(now_us-started==200000 && udp.tx_dropped_packets_==1 && !udp.tx_delivered_packets_);
    }
    {
        EspUdp udp; send_hook=busy; delay_hook=[&] { udp.connected_=false; };
        assert(udp.Send(packet)==-1 && disconnected==3); delay_hook=nullptr;
    }
    {
        EspUdp udp; int step=0,idle=0,messages=0,rx_disconnects=0;
        udp.receive_idle_callback_=[&] { ++idle; };
        udp.message_callback_=[&](const std::string& data) { assert(data=="abc"); ++messages; };
        udp.disconnected_callback_=[&] { ++rx_disconnects; };
        recv_hook=[&](char* data,size_t size) {
            assert(size>=3); ++step;
            if(step==1) { errno=EAGAIN; return -1; }
            if(step==2) { memcpy(data,"abc",3); return 3; }
            if(step==3) return 0;
            errno=ECONNRESET; return -1;
        };
        udp.ReceiveTask();
        assert(idle==1 && messages==1 && rx_disconnects==1 && !udp.connected_);
    }
    puts("PASS: real UDP 190 ms retry recovery, bounded retries, sustained loss detection, quiet TTS turns, shutdown and receive-idle expiry without losing datagrams");
}
'''


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    with tempfile.TemporaryDirectory(prefix="audio-uplink-") as directory:
        output = Path(directory)
        udp = function("components/78__esp-ml307/src/esp/esp_udp.cc", "int EspUdp::Send(")
        for name, content, libraries in (
            ("opus_batch", BATCH_TEST, ["-lopus"]),
            ("udp_send", UDP_PREFIX + udp + function("components/78__esp-ml307/src/esp/esp_udp.cc", "void EspUdp::ReceiveTask(") + UDP_TEST, []),
        ):
            source = output / (name + ".cc")
            source.write_text(content)
            executable = output / name
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-I", str(ROOT),
                "-I", str(ROOT / "managed_components/78__esp-opus/include"),
                str(source), "-o", str(executable), *libraries,
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
