#pragma once

#include "application.h"
#include "assets/lang_config.h"
#include "audio/codecs/vb_audio_codec.h"
#include "mcp_server.h"
#include "vb_adapter.h"
#include <esp_timer.h>

#include <cstring>
#include <mutex>
#include <string>

// The phone streams directly to the 701. Only one source may own its speaker.
class McpMusic {
public:
  static void Init(vbAudioCodec *codec) {
    auto &music = GetInstance();
    music.codec_ = codec;
    vb_event_register(
        VB_EVT_STATUS_CHANGE,
        [](uint32_t, void *data, uint16_t len, void *) {
          if (data && len == 1 && static_cast<uint8_t *>(data)[0] <= 1) {
            GetInstance().OnPlayStatus(static_cast<uint8_t *>(data)[0] == 1);
          }
        },
        nullptr);
    vb_event_register(
        VB_EVT_PLAY_STATUS_RESPONSE,
        [](uint32_t, void *data, uint16_t len, void *) {
          if (data && len == 1 && static_cast<uint8_t *>(data)[0] == 1) {
            // A snapshot may have been requested before a newer wake request.
            GetInstance().OnPhoneActivity("playback query", true, false);
          }
          // A query returning zero never clears the music ownership latch.
        },
        nullptr);
    vb_event_register(
        VB_EVT_BT_STATUS_CHANGE,
        [](uint32_t, void *data, uint16_t len, void *) {
          if (data && len) {
            // Stop on link changes without guessing the vendor's state values.
            // Disconnecting or pausing does not automatically resume old TTS.
            GetInstance().OnPhoneActivity("Bluetooth link change", false);
          }
        },
        nullptr);
    vb_event_register(
        VB_EVT_MODE_CHANGE,
        [](uint32_t, void *data, uint16_t len, void *) {
          if (data && len == 1 &&
              static_cast<uint8_t *>(data)[0] == VB_MUSIC_MODE_BT) {
            GetInstance().OnPhoneActivity("Bluetooth source selected", false);
          }
        },
        nullptr);
    for (auto event : {VB_EVT_MUSIC_TITLE, VB_EVT_MUSIC_LYRC}) {
      vb_event_register(
          event,
          [](uint32_t, void *data, uint16_t len, void *) {
            if (data && len && static_cast<const char *>(data)[0] != '\0') {
              GetInstance().OnPhoneActivity("phone media update", false);
            }
          },
          nullptr);
    }
    vb_event_register(
        VB_EVT_MUSIC_TIME,
        [](uint32_t, void *data, uint16_t len, void *) {
          if (data && len == 2 * sizeof(uint32_t)) {
            uint32_t time[2];
            std::memcpy(time, data, sizeof(time));
            GetInstance().OnMediaTime(time[0], time[1]);
          }
        },
        nullptr);
    // Catch music that was already playing when the ESP restarted.
    vb_api_request_play_status();
    if (!music.monitor_task_ && xTaskCreate(
                                    [](void *arg) {
                                      auto *self = static_cast<McpMusic *>(arg);
                                      while (true) {
                                        self->PollPlaybackStatus();
                                        vTaskDelay(pdMS_TO_TICKS(250));
                                      }
                                    },
                                    "701_play_status", 2048, &music, 3,
                                    &music.monitor_task_) != pdPASS) {
      ESP_LOGE("McpMusic", "Failed to start 701 playback monitor");
    }

    auto &server = McpServer::GetInstance();
    server.AddTool("self.bl_music.prev",
                   "通过701串口播放手机蓝牙音乐的上一首，同时结束小智对话。",
                   PropertyList(), [](const PropertyList &) -> ReturnValue {
                     GetInstance().StartMusic(Action::Previous);
                     return std::string("已请求701播放蓝牙音乐的上一首");
                   });
    server.AddTool("self.bl_music.next",
                   "通过701串口播放手机蓝牙音乐的下一首，同时结束小智对话。",
                   PropertyList(), [](const PropertyList &) -> ReturnValue {
                     GetInstance().StartMusic(Action::Next);
                     return std::string("已请求701播放蓝牙音乐的下一首");
                   });
    server.AddTool(
        "self.bl_music.set_play_or_pause",
        "控制手机直连701的蓝牙音乐。play_pause="
        "true时结束小智对话并继续播放音乐；"
        "play_pause="
        "false时暂停音乐。音乐与小智语音互斥，唤醒小智会先暂停音乐。",
        PropertyList({Property("play_pause", kPropertyTypeBoolean)}),
        [](const PropertyList &properties) -> ReturnValue {
          bool play = properties["play_pause"].value<bool>();
          if (play) {
            GetInstance().StartMusic(Action::Resume);
          } else {
            GetInstance().PauseMusic();
          }
          return std::string(play ? "已请求701继续播放蓝牙音乐"
                                  : "已请求701暂停音乐");
        });
  }

  static void WakeWord(const std::string &word) {
    auto &music = GetInstance();
    const auto generation = music.ReserveAction();
    // UART RX must stay available to receive the pause/query responses.
    Application::GetInstance().Schedule([word, generation]() {
      if (!GetInstance().PrepareChat(generation)) {
        return;
      }
      auto &app = Application::GetInstance();
      auto state = app.GetDeviceState();
      if (state == kDeviceStateUnknown || state == kDeviceStateStarting ||
          state == kDeviceStateWifiConfiguring) {
        app.PlaySound(Lang::Sounds::OGG_NET_DISCONNECT);
      } else {
        app.WakeWordInvoke(word);
      }
    });
  }

  static void ToggleChat() {
    const auto generation = GetInstance().ReserveAction();
    Application::GetInstance().Schedule([generation]() {
      auto &app = Application::GetInstance();
      if (!GetInstance().IsCurrentAction(generation)) {
        return;
      }
      if ((app.GetDeviceState() == kDeviceStateIdle || !app.IsChatEnabled()) &&
          !GetInstance().PrepareChat(generation)) {
        return;
      }
      app.ToggleChatState();
    });
  }

private:
  enum class Action { Previous, Next, Resume };
  vbAudioCodec *codec_ = nullptr;
  std::mutex mutex_;
  bool music_mode_ = false;
  bool music_playing_ = false;
  bool pause_observed_ = false;
  bool chat_requested_ = false;
  bool chat_handoff_ = false;
  bool media_time_valid_ = false;
  uint32_t media_time_ = 0;
  uint32_t media_duration_ = 0;
  uint32_t media_progress_revision_ = 0;
  uint32_t playback_event_revision_ = 0;
  TaskHandle_t monitor_task_ = nullptr;
  uint32_t status_revision_ = 0;
  uint32_t action_generation_ = 0;

  static McpMusic &GetInstance() {
    static McpMusic music;
    return music;
  }

  uint32_t ReserveAction() {
    std::lock_guard<std::mutex> lock(mutex_);
    chat_requested_ = true;
    return ++action_generation_;
  }

  bool IsCurrentAction(uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == action_generation_;
  }

  void OnPlayStatus(bool playing) {
    if (playing) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++playback_event_revision_;
      }
      OnPhoneActivity("playback event", true);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    music_playing_ = false;
    ++status_revision_;
    pause_observed_ = true;
    media_time_valid_ = false;
    ESP_LOGI("McpMusic", "701 pause event observed; waiting for stable status");
    // Pausing alone does not wake XiaoZhi or restore old TTS.
  }

  void OnPhoneActivity(const char *reason, bool playing,
                       bool cancel_pending_chat = true) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Metadata can be cached at a source transition. Actual playback evidence
    // still overrides pause verification while a wake handover is in progress.
    if (chat_handoff_ && !playing) {
      return;
    }
    if (playing) {
      music_playing_ = true;
      music_mode_ = true;
      pause_observed_ = false;
      ++status_revision_;
    }
    if (cancel_pending_chat && chat_requested_ && !chat_handoff_) {
      ++action_generation_;
      chat_requested_ = false;
    }
    auto &app = Application::GetInstance();
    const bool was_enabled = app.IsChatEnabled();
    // UART RX callback: close the PCM gate before deferring network cleanup.
    codec_->SetPlaybackBlocked(true);
    app.SetChatEnabled(false);
    if (was_enabled) {
      ESP_LOGI("McpMusic", "701 phone activity (%s): chat stopped until wake",
               reason);
    }
  }

  void OnMediaTime(uint32_t current, uint32_t total) {
    bool advancing;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (total == 0 || current > total) {
        ++media_progress_revision_; // Invalid telemetry cannot prove a fast pause.
        return;
      }
      advancing = media_time_valid_ && total == media_duration_ &&
                  current > media_time_;
      if (media_time_valid_ &&
          (current != media_time_ || total != media_duration_)) {
        ++media_progress_revision_;
      }
      media_time_valid_ = true;
      media_time_ = current;
      media_duration_ = total;
    }
    if (advancing) {
      OnPhoneActivity("media progress advancing", true);
    }
  }

  void PollPlaybackStatus() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (chat_handoff_ || !Application::GetInstance().IsChatEnabled()) {
        return;
      }
    }
    // Asynchronous request: never wait for UART replies from the RX callback,
    // and do not occupy the protocol's synchronous request slot.
    vb_api_request_play_status();
  }

  void StartMusic(Action action) {
    uint32_t generation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      generation = ++action_generation_;
      chat_requested_ = false;
      music_mode_ = true;
      pause_observed_ = false;
      codec_->SetPlaybackBlocked(true);
    }
    auto &app = Application::GetInstance();
    // Let the MCP result leave on the current session before closing it.
    app.Schedule([this, generation, action]() {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != action_generation_) {
          return;
        }
      }
      Application::GetInstance().SetChatEnabled(false);
      vb_api_set_music_mode(VB_MUSIC_MODE_BT);
      bool bluetooth_selected = false;
      for (int attempt = 0; attempt < 5; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(100));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (generation != action_generation_) {
            return;
          }
        }
        uint8_t mode = VB_MUSIC_MODE_MAX;
        if (vb_api_query_music_mode(&mode) == 0 && mode == VB_MUSIC_MODE_BT) {
          bluetooth_selected = true;
          break;
        }
      }
      if (!bluetooth_selected) {
        ESP_LOGW("McpMusic",
                 "701 Bluetooth source not confirmed; music command cancelled");
        return;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != action_generation_) {
        return;
      }
      if (action != Action::Resume) {
        vb_api_set_music_next_prev(action == Action::Next ? 1 : 0);
      }
      vb_api_set_music_play(1);
    });
  }

  void PauseMusic() {
    const auto generation = ReserveAction();
    Application::GetInstance().Schedule(
        [this, generation]() { PrepareChat(generation); });
  }

  // Main event task only. A timeout is not evidence that music has paused.
  bool PrepareChat(uint32_t generation) {
    auto &app = Application::GetInstance();
    bool discard_chat;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != action_generation_) {
        return false;
      }
      discard_chat = music_mode_ || !app.IsChatEnabled();
      chat_handoff_ = true;
      codec_->SetPlaybackBlocked(true);
    }
    struct HandoffScope {
      McpMusic *self;
      ~HandoffScope() {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->chat_handoff_ = false;
      }
    } handoff{this};
    if (discard_chat) {
      app.SetChatEnabled(false);
    }

    // A single zero query was previously observed while music was audible.
    // The fast path requires a verified 1 -> 0 transition and several fresh
    // zero responses. A stream that always returns zero cannot qualify.
    uint8_t before_pause = 0;
    const bool queried_playing = discard_chat &&
        vb_api_query_play_status(&before_pause) == 0 && before_pause == 1;
    const int64_t pause_requested_us = esp_timer_get_time();
    uint32_t playing_event_revision;
    uint32_t playing_revision;
    uint32_t progress_revision;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      playing_revision = status_revision_;
      progress_revision = media_progress_revision_;
      playing_event_revision = playback_event_revision_;
    }
    // Call only with mutex_ held, including immediately before reopening PCM.
    auto fast_pause_confirmed = [&]() {
      return queried_playing && playback_event_revision_ == playing_event_revision &&
             status_revision_ == playing_revision &&
             media_progress_revision_ == progress_revision &&
             esp_timer_get_time() - pause_requested_us >= 500000;
    };
    // On this 701 the actual pause is immediate, but its notification arrives
    // seconds later. Keep receiving progress/status and send just one pause.
    vb_api_set_music_play(0);
    vTaskDelay(pdMS_TO_TICKS(150));

    auto wait_for_pause = [&]() {
      int stable_paused = 0;
      while (esp_timer_get_time() - pause_requested_us < 7000000) {
        uint32_t revision;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (generation != action_generation_) {
            return false;
          }
          revision = status_revision_;
        }
        // Use the delayed event unless the independent fast-path evidence agrees.
        uint8_t status = 1;
        const bool valid_status = vb_api_query_play_status(&status) == 0;
        bool paused = valid_status && status == 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (generation != action_generation_) {
            return false;
          }
          if (valid_status && status == 1 && revision == status_revision_) {
            music_mode_ = true;
            music_playing_ = true;
            pause_observed_ = false;
          }
          discard_chat = discard_chat || music_mode_ || !app.IsChatEnabled();
          // A newer 'playing' report overrides an older query response.
          paused = paused && (revision == status_revision_ || !music_playing_);
          if ((playing_revision != status_revision_ && music_playing_) ||
              progress_revision != media_progress_revision_) {
            // Drain a late progress packet by restarting the consecutive-query
            // check. A new playback event still invalidates the fast path.
            stable_paused = 0;
          }
          playing_revision = status_revision_;
          progress_revision = media_progress_revision_;
          stable_paused = paused ? stable_paused + 1 : 0;
          paused = paused && ((!music_mode_ || pause_observed_) ||
                              (stable_paused >= 3 && fast_pause_confirmed()));
          if (paused) {
            music_playing_ = false;
          }
        }
        if (paused && stable_paused >= 2) {
          bool drained = true;
          if (discard_chat) {
            // Finish cleanup even if its UART-scheduled callback has not run
            // yet.
            app.SetChatEnabled(false);
            if (app.GetAudioService().IsInitialize()) {
              drained = app.GetAudioService().WaitForPlayCompletion(200);
            }
          }
          std::lock_guard<std::mutex> lock(mutex_);
          if (generation == action_generation_ && drained && !music_playing_ &&
              (!music_mode_ || pause_observed_ || fast_pause_confirmed())) {
            music_mode_ = false;
            app.SetChatEnabled(true);
            codec_->SetPlaybackBlocked(false);
            ESP_LOGI("McpMusic", "701 pause confirmed (%s, %d ms): chat enabled",
                     pause_observed_ ? "event/status" :
                     queried_playing ? "status transition/queries" : "idle/status",
                     static_cast<int>((esp_timer_get_time() - pause_requested_us) / 1000));
            return true;
          }
        }
        // Allow the phone time to apply the Bluetooth pause command.
        vTaskDelay(pdMS_TO_TICKS(queried_playing && stable_paused > 0 ? 100 : 200));
      }
      return false;
    };

    if (wait_for_pause()) {
      return true;
    }
    app.SetChatEnabled(false);
    ESP_LOGW(
        "McpMusic",
        "701 pause not confirmed within 7 s: keeping chat output blocked");
    return false;
  }
};
