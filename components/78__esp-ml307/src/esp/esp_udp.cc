#include "esp_udp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <cerrno>
#include <esp_timer.h>

static const char *TAG = "EspUdp";

EspUdp::EspUdp() : udp_fd_(-1) {
    event_group_ = xEventGroupCreate();
}

EspUdp::~EspUdp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspUdp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (udp_fd_ != -1) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(udp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    // Periodically check for shutdown. A quiet UDP peer is still connected.
    struct timeval tv;
    tv.tv_sec = receive_idle_callback_ ? 0 : 1;
    tv.tv_usec = receive_idle_callback_ ? 20000 : 0;
    if (setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGW(TAG, "Failed to set socket receive timeout");
    }

    tx_congestion_since_us_ = 0;
    tx_stable_since_us_ = 0;
    tx_last_success_us_ = 0;
    tx_last_send_us_ = 0;
    tx_dropped_packets_ = 0;
    tx_delivered_packets_ = 0;
    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t created = xTaskCreate([](void* arg) {
        EspUdp* udp = (EspUdp*)arg;
        udp->ReceiveTask();
        xEventGroupSetBits(udp->event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
#ifdef CONFIG_IDF_TARGET_ESP32C2
    }, "udp_receive", 2048, this, 5, &receive_task_handle_);
#else
    }, "udp_receive", 4096, this, 5, &receive_task_handle_);
#endif
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UDP receive task");
        connected_ = false;
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }
    return true;
}

void EspUdp::Disconnect() {
    connected_ = false;

    if (udp_fd_ != -1) {
        close(udp_fd_);
        udp_fd_ = -1;

        auto bits = xEventGroupWaitBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_UDP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
    }
}

int EspUdp::Send(const std::string& data) {
    if (!connected_) {
        return -1;
    }

    int error = 0;
    // A 30 ms retry budget discarded much of a spoken phrase during TX stalls.
    // Yield to Wi-Fi for up to 200 ms, keeping this datagram intact. The audio
    // sender has its own bounded queue and rejects packets from old sessions.
    const int64_t started_us = esp_timer_get_time();
    if (tx_last_send_us_ != 0 && started_us - tx_last_send_us_ > 1000000) {
        // The same socket is quiet while TTS plays. Silence between listening
        // turns must not count as time spent failing to send microphone audio.
        tx_congestion_since_us_ = 0;
        tx_stable_since_us_ = 0;
        tx_last_success_us_ = 0;
        tx_dropped_packets_ = 0;
        tx_delivered_packets_ = 0;
    }
    tx_last_send_us_ = started_us;
    int backoff_ms = 10;
    while (connected_) {
        int ret = send(udp_fd_, data.data(), data.size(), MSG_DONTWAIT);
        if (ret == static_cast<int>(data.size())) {
            const int64_t now = esp_timer_get_time();
            tx_last_success_us_ = now;
            if (tx_congestion_since_us_ != 0) {
                ++tx_delivered_packets_;
                if (tx_stable_since_us_ == 0) {
                    tx_stable_since_us_ = now;
                }
                // One successful send does not establish a healthy uplink.
                if (now - tx_stable_since_us_ >= 1000000) {
                    if (tx_dropped_packets_ > 0) {
                        ESP_LOGI(TAG, "UDP TX stable for 1 s; episode delivered=%u, dropped=%u",
                            tx_delivered_packets_, tx_dropped_packets_);
                    }
                    tx_congestion_since_us_ = 0;
                    tx_stable_since_us_ = 0;
                    tx_dropped_packets_ = 0;
                    tx_delivered_packets_ = 0;
                }
            }
            return ret;
        }
        error = ret < 0 ? errno : EIO;
        if (error != ENOMEM && error != ENOBUFS && error != EAGAIN &&
            error != EWOULDBLOCK && error != EINTR) {
            break;
        }
        const int64_t now = esp_timer_get_time();
        if (tx_congestion_since_us_ == 0) {
            tx_congestion_since_us_ = now;
        }
        tx_stable_since_us_ = 0;
        if (tx_last_success_us_ == 0) {
            tx_last_success_us_ = now;
        }
        const int64_t remaining_us = 200000 - (now - started_us);
        if (remaining_us <= 0) {
            break;
        }
        const int wait_ms = (remaining_us + 999) / 1000;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms < wait_ms ? backoff_ms : wait_ms));
        backoff_ms = backoff_ms < 40 ? backoff_ms * 2 : 40;
    }
    if (!connected_) {
        return -1;
    }
    const bool transient = error == ENOMEM || error == ENOBUFS || error == EAGAIN ||
        error == EWOULDBLOCK || error == EINTR;
    if (transient) {
        ++tx_dropped_packets_;
        const int64_t now = esp_timer_get_time();
        const int64_t last_progress = tx_last_success_us_ > tx_congestion_since_us_ ?
            tx_last_success_us_ : tx_congestion_since_us_;
        const bool stalled = now - last_progress >= 2000000;
        const bool excessive_loss = now - tx_congestion_since_us_ >= 3000000 &&
            tx_dropped_packets_ >= 3 && tx_dropped_packets_ >= tx_delivered_packets_;
        if (!stalled && !excessive_loss) {
            if (tx_dropped_packets_ == 1) {
                ESP_LOGW(TAG, "UDP TX still busy after 200 ms (errno=%d); audio packet could not be delivered", error);
            }
            return 0;
        }
    }
    ESP_LOGE(TAG, "UDP TX unavailable (errno=%d, delivered=%u, dropped=%u), closing audio channel",
        error, tx_delivered_packets_, tx_dropped_packets_);
    if (connected_.exchange(false) && disconnected_callback_) {
        disconnected_callback_();
    }
    return -1;
}

void EspUdp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(udp_fd_, data.data(), data.size(), 0);
        if (!connected_) {
            break;
        }
        if (ret < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
                if (receive_idle_callback_) {
                    receive_idle_callback_();
                }
                continue;
            }
            ESP_LOGE(TAG, "UDP receive failed: %d, errno=%d, connection lost", ret, error);
            if (connected_.exchange(false) && disconnected_callback_) {
                disconnected_callback_();
            }
            break;
        }
        if (ret == 0) {
            // A zero-length datagram does not close a UDP socket.
            continue;
        }

        if (message_callback_) {
            data.resize(ret);
            message_callback_(data);
        }
    }
}
