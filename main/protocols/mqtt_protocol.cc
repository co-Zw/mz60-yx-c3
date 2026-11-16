#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <arpa/inet.h>
#include <soc/soc_caps.h>
#if SOC_WIFI_SUPPORTED
#include <esp_wifi.h>
#endif
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                app.Schedule([protocol]() {
                    protocol->StartMqttClient(false);
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr || !udp_->IsConnected()) {
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet->payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) == static_cast<int>(encrypted.size());
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        LogReceiveStats(esp_timer_get_time(), true);
        incoming_audio_.Reset();
        reported_rx_stats_ = {};
        last_rx_stats_us_ = 0;
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    // Stop the previous receiver before the new hello changes its AES context
    // and sequence state. Destruction joins the old receive task.
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnDisconnected([this, session = session_id_]() {
        // Destruction waits for the receive task, so close on the main task.
        Application::GetInstance().Schedule([this, session]() {
            if (session_id_ == session && udp_ != nullptr && !udp_->IsConnected()) {
                CloseAudioChannel();
                SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            }
        });
    });
    udp_->OnMessage([this](const std::string& data) {
        HandleAudioPacket(data);
    });
    udp_->OnReceiveIdle([this]() {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        DrainIncomingAudio(esp_timer_get_time());
    });

    if (!udp_->Connect(udp_server_, udp_port_)) {
        udp_.reset();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
#if SOC_WIFI_SUPPORTED
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "Audio Wi-Fi link: SSID=%s, BSSID=%02x:%02x:%02x:%02x:%02x:%02x, RSSI=%d dBm, channel=%u",
                 reinterpret_cast<const char*>(ap.ssid),
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5], ap.rssi, ap.primary);
    }
#endif
    return true;
}

void MqttProtocol::HandleAudioPacket(const std::string& data) {
    // type, flags, payload length, SSRC, timestamp, sequence (16-byte header).
    constexpr size_t header_size = 16;
    if (data.size() < header_size || aes_nonce_.size() != header_size) {
        ESP_LOGE(TAG, "Invalid audio packet size: %u", (unsigned)data.size());
        return;
    }
    if (data[0] != 0x01) {
        ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
        return;
    }

    uint16_t payload_length;
    uint32_t timestamp, sequence;
    memcpy(&payload_length, data.data() + 2, sizeof(payload_length));
    memcpy(&timestamp, data.data() + 8, sizeof(timestamp));
    memcpy(&sequence, data.data() + 12, sizeof(sequence));
    timestamp = ntohl(timestamp);
    sequence = ntohl(sequence);
    if (ntohs(payload_length) != data.size() - header_size) {
        ESP_LOGW(TAG, "Invalid audio payload length");
        return;
    }
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->timestamp = timestamp;
    packet->payload.resize(data.size() - header_size);
    size_t nc_off = 0;
    uint8_t nonce[header_size], stream_block[16] = {0};
    memcpy(nonce, data.data(), header_size);
    int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off,
        nonce, stream_block, reinterpret_cast<const uint8_t*>(data.data() + header_size),
        packet->payload.data());
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
        return;
    }

    std::lock_guard<std::mutex> lock(incoming_mutex_);
    const int64_t now_us = esp_timer_get_time();
    // Insert first: an expected packet arriving exactly at the gap deadline
    // must be accepted before deciding whether to conceal it. Pop also drains
    // on window pressure, so a burst cannot grow this window without bounds.
    incoming_audio_.Push(sequence, std::move(packet), now_us);
    DrainIncomingAudio(now_us);
    last_incoming_time_ = std::chrono::steady_clock::now();
}

// incoming_mutex_ serializes both removal and callback delivery. Releasing it
// before delivery would let the tail flush overtake the UDP receive task.
void MqttProtocol::DrainIncomingAudio(int64_t now_us, bool force) {
    while (true) {
        auto received = incoming_audio_.Pop(now_us, force);
        if (!received.packet) {
            break;
        }
        auto packet = std::move(received.packet);
        if (on_incoming_audio_ == nullptr) {
            continue;
        }
        // The next Opus packet can carry FEC for its immediate predecessor.
        // Earlier losses use PLC; cap recovery at 180 ms rather than creating
        // an unbounded backlog. FEC falls back to PLC when unavailable.
        const uint32_t missing = received.missing;
        if (missing > 0 && missing <= 3) {
            for (int i = missing; i > 0; --i) {
                auto concealed = std::make_unique<AudioStreamPacket>();
                concealed->sample_rate = packet->sample_rate;
                concealed->frame_duration = packet->frame_duration;
                const uint32_t offset = i * packet->frame_duration;
                concealed->timestamp = packet->timestamp >= offset ? packet->timestamp - offset : 0;
#ifndef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
                if (i == 1) {
                    concealed->payload = packet->payload;
                    concealed->decode_fec = true;
                }
#endif
                on_incoming_audio_(std::move(concealed));
            }
        }
        on_incoming_audio_(std::move(packet));
    }
    LogReceiveStats(now_us, force);
}

void MqttProtocol::FlushIncomingAudio() {
    // MQTT stop can overtake the last UDP datagrams on a weak link. Let them
    // enter the same receive window before the application ends playback.
    vTaskDelay(pdMS_TO_TICKS(120));
    std::lock_guard<std::mutex> lock(incoming_mutex_);
    DrainIncomingAudio(esp_timer_get_time(), true);
}

void MqttProtocol::LogReceiveStats(int64_t now_us, bool force) {
    const auto& stats = incoming_audio_.stats();
    if (stats.delivered == 0 && stats.missing == 0 && stats.overflow == 0) {
        return;
    }
    if (!force && stats.missing == reported_rx_stats_.missing && stats.reordered == reported_rx_stats_.reordered &&
        stats.late == reported_rx_stats_.late && stats.overflow == reported_rx_stats_.overflow) {
        return;
    }
    if (!force && last_rx_stats_us_ != 0 && now_us - last_rx_stats_us_ < 5000000) {
        return;
    }
    int rssi = 0;
#if SOC_WIFI_SUPPORTED
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
#endif
    if (stats.missing != reported_rx_stats_.missing || stats.late != reported_rx_stats_.late ||
        stats.overflow != reported_rx_stats_.overflow) {
        ESP_LOGW(TAG, "Audio RX: delivered=%u, reordered=%u, missing=%u, late=%u, window_full=%u, RSSI=%d dBm",
                 stats.delivered, stats.reordered, stats.missing, stats.late, stats.overflow, rssi);
    } else {
        ESP_LOGI(TAG, "Audio RX: delivered=%u, reordered=%u, missing=%u, RSSI=%d dBm",
                 stats.delivered, stats.reordered, stats.missing, rssi);
    }
    reported_rx_stats_ = stats;
    last_rx_stats_us_ = now_us;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        incoming_audio_.Reset();
        reported_rx_stats_ = {};
        last_rx_stats_us_ = 0;
    }
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr && udp_->IsConnected() && !error_occurred_ && !IsTimeout();
}
