#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include "audio_reorder_buffer.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool SendText(const std::string& text) override;
    void FlushIncomingAudio() override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    mutable std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    std::mutex incoming_mutex_;
    AudioReorderBuffer<AudioStreamPacket> incoming_audio_;
    AudioReorderBuffer<AudioStreamPacket>::Stats reported_rx_stats_;
    int64_t last_rx_stats_us_ = 0;
    esp_timer_handle_t reconnect_timer_;

    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    void HandleAudioPacket(const std::string& data);
    void DrainIncomingAudio(int64_t now_us, bool force = false);
    void LogReceiveStats(int64_t now_us, bool force = false);
    std::string DecodeHexString(const std::string& hex_string);

    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
