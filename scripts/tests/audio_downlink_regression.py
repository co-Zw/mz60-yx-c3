#!/usr/bin/env python3
"""Native reorder-window and actual MQTT receive-path fault injection.

Requires g++. AES is an identity stub here; this tests packet validation,
ordering, bounded concealment and tail flushing, not cryptography or RF loss.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PREFIX = r'''
#include "main/protocols/audio_reorder_buffer.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <arpa/inet.h>
struct AudioStreamPacket {
    int sample_rate=0,frame_duration=0;
    uint32_t timestamp=0;
    std::vector<uint8_t> payload;
    bool decode_fec=false;
};
template<class... Args> void discard_log(Args&&...) {}
#define ESP_LOGI(...) discard_log(__VA_ARGS__)
#define ESP_LOGW(...) discard_log(__VA_ARGS__)
#define ESP_LOGE(...) discard_log(__VA_ARGS__)
#define SOC_WIFI_SUPPORTED 0
#define pdMS_TO_TICKS(x) (x)
static const char* TAG="MQTT";
static int64_t now_us=1000000;
static bool decrypt_fails=false;
static std::function<void()> delay_hook;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(int ms) { assert(ms==120); now_us+=ms*1000; if(delay_hook) delay_hook(); }
int mbedtls_aes_crypt_ctr(int*,size_t size,size_t*,uint8_t*,uint8_t*,const uint8_t* input,uint8_t* output) {
    if(decrypt_fails) return -1;
    memcpy(output,input,size); return 0;
}
struct MqttProtocol {
    std::string aes_nonce_=std::string(16,'a');
    int aes_ctx_=0,server_sample_rate_=24000,server_frame_duration_=60;
    std::mutex incoming_mutex_;
    AudioReorderBuffer<AudioStreamPacket> incoming_audio_;
    AudioReorderBuffer<AudioStreamPacket>::Stats reported_rx_stats_;
    int64_t last_rx_stats_us_=0;
    std::function<void(std::unique_ptr<AudioStreamPacket>)> on_incoming_audio_;
    std::chrono::steady_clock::time_point last_incoming_time_;
    void HandleAudioPacket(const std::string&);
    void DrainIncomingAudio(int64_t,bool=false);
    void FlushIncomingAudio();
    void LogReceiveStats(int64_t,bool=false);
};
'''

TEST = r'''
std::unique_ptr<int> value(int n) { return std::make_unique<int>(n); }
std::string wire(uint32_t sequence) {
    std::string data(18,'\0'); data[0]=1;
    uint16_t size=htons(2); uint32_t seq=htonl(sequence),ts=htonl(sequence*60);
    memcpy(data.data()+2,&size,2); memcpy(data.data()+8,&ts,4); memcpy(data.data()+12,&seq,4);
    data[16]=sequence & 255; data[17]='a'; return data;
}
int main() {
    using Window=AudioReorderBuffer<int>;
    {
        Window window;
        assert(window.Push(2,value(2),0));
        assert(window.Push(1,value(1),20000));
        assert(!window.Pop(59999).packet);
        auto one=window.Pop(60000),two=window.Pop(60000);
        assert(one.sequence==1 && two.sequence==2 && !one.missing && !two.missing);
        assert(window.stats().reordered==1 && !window.stats().missing);
        assert(window.Push(4,value(4),61000));
        assert(!window.Pop(180999).packet);
        assert(window.Push(3,value(3),181000));
        auto three=window.Pop(181000),four=window.Pop(181000);
        assert(three.sequence==3 && four.sequence==4 && !four.missing);
        assert(window.Push(6,value(6),200000));
        assert(!window.Pop(319999).packet);
        auto six=window.Pop(320000); assert(six.sequence==6 && six.missing==1);
        assert(!window.Push(5,value(5),330000));
        assert(!window.Push(6,value(6),330000));
        assert(window.stats().late==2 && window.stats().missing==1);
    }
    {
        // Every permutation within four-packet bursts must preserve all audio.
        Window window; std::vector<uint32_t> delivered; std::mt19937 random(217);
        int64_t time=0;
        for(uint32_t base=1;base<=400;base+=4) {
            std::vector<uint32_t> batch{base,base+1,base+2,base+3};
            std::shuffle(batch.begin(),batch.end(),random);
            for(auto seq:batch) {
                assert(window.Push(seq,value(seq),time)); time+=5000;
                while(true) { auto item=window.Pop(time); if(!item.packet) break; delivered.push_back(item.sequence); }
                assert(window.size()<=Window::kCapacity);
            }
        }
        while(true) { auto item=window.Pop(time,true); if(!item.packet) break; delivered.push_back(item.sequence); }
        assert(delivered.size()==400 && !window.stats().missing && !window.stats().overflow);
        for(uint32_t i=0;i<400;++i) assert(delivered[i]==i+1);
    }
    {
        Window window; assert(window.Push(0xfffffffe,value(1),0));
        assert(window.Pop(60000).sequence==0xfffffffe);
        assert(window.Push(0,value(3),70000)); assert(!window.Pop(70000).packet);
        assert(window.Push(0xffffffff,value(2),80000));
        assert(window.Pop(80000).sequence==0xffffffff && window.Pop(80000).sequence==0);
        assert(!window.stats().missing);
        window.Reset(); assert(window.Push(500,value(500),90000));
        assert(window.Pop(90000,true).sequence==500 && !window.stats().missing);
    }
    {
        Window window;
        for(uint32_t seq=1;seq<=Window::kCapacity;++seq) assert(window.Push(seq,value(seq),0));
        assert(!window.Push(9,value(9),0) && window.stats().overflow==1);
        assert(window.Pop(0).packet); // pressure drains even during startup
        window.Reset(); assert(!window.size() && !window.stats().overflow);
    }
    {
        MqttProtocol mqtt; std::vector<AudioStreamPacket> output;
        mqtt.on_incoming_audio_=[&](auto packet) { output.push_back(std::move(*packet)); };
        mqtt.HandleAudioPacket(wire(1)); assert(output.empty());
        now_us+=60000; mqtt.DrainIncomingAudio(now_us); assert(output.size()==1);
        now_us+=1000; mqtt.HandleAudioPacket(wire(3)); assert(output.size()==1);
        now_us+=70000; mqtt.HandleAudioPacket(wire(2));
        assert(output.size()==3 && output[1].payload[0]==2 && output[2].payload[0]==3);
        assert(!mqtt.incoming_audio_.stats().missing && mqtt.incoming_audio_.stats().reordered==1);
        mqtt.HandleAudioPacket(wire(2)); assert(output.size()==3);
        mqtt.HandleAudioPacket(wire(5)); now_us+=120000; mqtt.DrainIncomingAudio(now_us);
        assert(output.size()==5 && output[3].decode_fec && output[3].timestamp==240);
        assert(output[3].payload==output[4].payload && !output[4].decode_fec);
        assert(output[3].frame_duration==60 && output[4].payload[0]==5);
        // Large gaps never synthesize an unbounded PLC backlog.
        mqtt.HandleAudioPacket(wire(100)); now_us+=120000; mqtt.DrainIncomingAudio(now_us);
        assert(output.size()==6 && output.back().payload[0]==100);
        auto malformed=wire(101); malformed[0]=2; mqtt.HandleAudioPacket(malformed);
        malformed=wire(101); malformed[3]=99; mqtt.HandleAudioPacket(malformed);
        decrypt_fails=true; mqtt.HandleAudioPacket(wire(101)); decrypt_fails=false;
        assert(output.size()==6 && !mqtt.incoming_audio_.size());
    }
    {
        // Only the loss immediately before the received packet uses its FEC.
        MqttProtocol mqtt; std::vector<AudioStreamPacket> output;
        mqtt.on_incoming_audio_=[&](auto packet) { output.push_back(std::move(*packet)); };
        mqtt.HandleAudioPacket(wire(1)); now_us+=60000; mqtt.DrainIncomingAudio(now_us);
        mqtt.HandleAudioPacket(wire(5)); now_us+=120000; mqtt.DrainIncomingAudio(now_us);
        assert(output.size()==5 && output[1].payload.empty() && output[2].payload.empty());
        assert(!output[1].decode_fec && !output[2].decode_fec && output[3].decode_fec);
        assert(output[3].payload==output[4].payload && !output[4].decode_fec);
    }
    {
        // MQTT stop may arrive before the reordered UDP tail. The flush gives
        // the receive task a chance to deliver the missing original first.
        MqttProtocol mqtt; std::vector<int> heard;
        mqtt.on_incoming_audio_=[&](auto packet) { heard.push_back(packet->payload.empty() ? -1 : packet->payload[0]); };
        mqtt.HandleAudioPacket(wire(10)); now_us+=60000; mqtt.DrainIncomingAudio(now_us);
        mqtt.HandleAudioPacket(wire(12));
        delay_hook=[&] { mqtt.HandleAudioPacket(wire(11)); };
        mqtt.FlushIncomingAudio(); delay_hook=nullptr;
        assert((heard==std::vector<int>{10,11,12}) && !mqtt.incoming_audio_.stats().missing);
    }
    puts("PASS: startup/stream reorder, 120 ms loss deadline, bounded memory/PLC, late duplicates, sequence wrap, 400-packet permutations, real MQTT parsing and delayed TTS tail");
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
    with tempfile.TemporaryDirectory(prefix="audio-downlink-") as directory:
        output = Path(directory)
        methods = "".join(function("main/protocols/mqtt_protocol.cc", signature) for signature in (
            "void MqttProtocol::HandleAudioPacket(", "void MqttProtocol::DrainIncomingAudio(",
            "void MqttProtocol::FlushIncomingAudio(", "void MqttProtocol::LogReceiveStats(",
        ))
        source = output / "downlink.cc"
        source.write_text(PREFIX + methods + TEST)
        executable = output / "downlink"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
            "-I", str(ROOT), str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
