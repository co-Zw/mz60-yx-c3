
#include "application.h"
#include "audio/codecs/vb_audio_codec.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "hal/gpio_types.h"
#include "led/single_led.h"
#include "soc/gpio_num.h"

#include "wifi_board.h"

#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <wifi_station.h>
#include <string_view>

#include <ssid_manager.h>
#include <wifi_configuration_ap.h>
#include <wifi_station.h>

#include "assets/lang_config.h"
#include "settings.h"

#include "power_save_timer.h"

#include "esp_timer.h"

#include "../firmware/_common_v2/utils/power_manager.h"

#include "mcp_alarm.hpp"
#include "mcp_ota_checker.hpp"
#include "mcp_power_manager.hpp"
#include "mcp_music.hpp"

#include "vb_adapter.h"

#define TAG "Mz01C3Lcd"

class Mz01C3Lcd : public WifiBoard {
private:
  Button *boot_button_ = nullptr;
  Button *volume_up_button_ = nullptr;
  Button *volume_down_button_ = nullptr;
  vbAudioCodec *audio_codec_ = nullptr;
  PowerSaveTimer *power_save_timer_ = nullptr;
  PowerManager *power_manager_ = nullptr;
  esp_timer_handle_t boot_sound_timer_ = nullptr;
  Led *led_ = nullptr;

#if defined(CONFIG_USE_DEVICE_AEC)
  int volume_up_press = 0;
  int volume_down_press = 0;
  int volume_up_down_press = 0;

  void ToggleRealtimeChatMode() {

    auto &app = Application::GetInstance();
    app.SetAecMode((app.GetAecMode() == kAecOff) ? kAecOnDeviceSide : kAecOff);
    Settings settings("realtime", true);
    settings.SetInt("enable", (app.GetAecMode() == kAecOff) ? 0 : 1);
    ESP_LOGI(TAG, "set realtime enable: %d",
             (app.GetAecMode() == kAecOff) ? 0 : 1);
    app.Schedule([]() {
      auto &app = Application::GetInstance();
      // app.Close();
      app.Schedule([]() {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) {
          auto display = Board::GetInstance().GetDisplay();
          if (app.GetAecMode() == kAecOff) {
            // display->ShowNotification(Lang::Strings::WAKEUP_WORD_ABORT,
            // 1000);
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_WAKEWORD);
          } else {
            // display->ShowNotification(Lang::Strings::REALTIME_ABORT, 1000);
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_REALTIME);
          }
        } else {
          app.Schedule([]() {
            auto &app = Application::GetInstance();
            auto display = Board::GetInstance().GetDisplay();
            if (app.GetAecMode() == kAecOff) {
              // display->ShowNotification(Lang::Strings::WAKEUP_WORD_ABORT,
              // 1000);
              Application::GetInstance().PlaySound(Lang::Sounds::OGG_WAKEWORD);
            } else {
              // display->ShowNotification(Lang::Strings::REALTIME_ABORT, 1000);
              Application::GetInstance().PlaySound(Lang::Sounds::OGG_REALTIME);
            }
          });
        }
      });
    });
  }
#endif

  void ShutDown() {
    ESP_LOGI(TAG, "Shutting down");
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_direction(POWER_KEEP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(POWER_KEEP_GPIO, GPIO_PULLDOWN_ONLY);
    gpio_set_level(POWER_KEEP_GPIO, POWER_KEEP_LEVEL ? 0 : 1);
  }

  void InitializePowerManager() {

    gpio_num_t charge_gpio = BATTERY_CHARG_GPIO;
    gpio_num_t adc_gpio = BATTERY_ADC_GPIO;
    int voltage_max = (int)BATTERY_VOLTAGE_MAX;
    int voltage_min = (int)BATTERY_VOLTAGE_MIN;

    if (adc_gpio != GPIO_NUM_NC && (voltage_max <= voltage_min)) {
      ESP_LOGE(TAG, "voltage config error");
      return;
    }
    if (charge_gpio != GPIO_NUM_NC || adc_gpio != GPIO_NUM_NC) {
      power_manager_ =
          new PowerManager(charge_gpio, adc_gpio, voltage_max, voltage_min);
      power_manager_->OnChargingStatusChanged([this](bool is_charging) {
        // if (is_charging) {
        //   power_save_timer_->SetEnabled(false);
        // } else {
        //   power_save_timer_->SetEnabled(true);
        // }
      });
      power_manager_->OnLowBatteryStatusChanged([this](bool is_low) {
        if (power_manager_->IsCharging() == false) { // 低电量时提示音
          Application::GetInstance().PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
        }
      });
      power_manager_->OnChargingLevelChanged([this](uint32_t level) {
        if (power_manager_->IsCharging() == false && level <= 5) {
          // 电量低于5时直接关机
          ShutDown();
        }
      });
    }
  }

  void InitializePowerSaveTimer() {
    gpio_num_t keep_gpio = POWER_KEEP_GPIO;
    int keep_level = (int)POWER_KEEP_LEVEL;
    int sleep_time = (int)POWER_SLEEP_TIME;
    int shutdown_time = (int)POWER_SHUTDOWN_TIME;

    if (keep_gpio != GPIO_NUM_NC) {
      gpio_set_direction(keep_gpio, GPIO_MODE_OUTPUT);
      gpio_set_pull_mode(keep_gpio, GPIO_PULLUP_ONLY);
      gpio_set_level(keep_gpio, keep_level);

      power_save_timer_ = new PowerSaveTimer(-1, sleep_time, shutdown_time);
      power_save_timer_->OnEnterSleepMode([this]() {
        if (McpPowerManager::GetInstance()->GetIsAutoPoweroff()) {
          ESP_LOGW(TAG, "Enabling sleep mode");
          // display_->SetChatMessage("system", "");
          // display_->SetEmotion("sleepy");
          // GetBacklight()->SetBrightness(10);
        }
      });
      power_save_timer_->OnExitSleepMode([this, keep_gpio, keep_level]() {
        ESP_LOGW(TAG, "Enabling sleep mode");
        gpio_set_level(keep_gpio, keep_level);
        // display_->SetChatMessage("system", "");
        // display_->SetEmotion("neutral");
        // GetBacklight()->RestoreBrightness();
      });
      power_save_timer_->OnShutdownRequest([this, keep_gpio, keep_level]() {
        if (McpPowerManager::GetInstance()->GetIsAutoPoweroff() &&
            power_manager_->IsCharging() == false) {
          ShutDown();
        }
      });
      power_save_timer_->SetEnabled(true);
    }
  }

  void InitializeButtons() {
    if (boot_button_) {
      boot_button_->OnClick([this]() {
        // if (audio_codec_->InOtaMode(1) == true) {
        //     ESP_LOGI(TAG, "OTA mode, do not enter chat");
        //     return;
        // }
        if (McpAlarm::GetInstance()->IsAlarm()) {
          McpAlarm::GetInstance()->StopAlarm();
        }
        // if (power_save_timer_) {
        //   power_save_timer_->WakeUp();
        // }
        McpMusic::ToggleChat();
      });
      boot_button_->OnPressRepeaDone([this](uint16_t count) {
        if (count == 1) {
          return;
        }

        // if (audio_codec_->InOtaMode(1) == true) {
        //     ESP_LOGI(TAG, "OTA mode, do not enter chat");
        //     return;
        // }

        // #if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT
        // == 1
        //             if(count >= 7){
        //                 if(GetBoardCfg()->GetFunEnable(kFunEnableVb6824OTA)){
        //                     int ret = audio_codec_->OtaStart(0);
        //                     if(ret == vbAudioCodec::OTA_ERR_NOT_SUPPORT){
        //                         ESP_LOGW(TAG, "Please enable
        //                         VB6824_OTA_SUPPORT");
        //                     }
        //                 }
        //             }else
        // #endif

#ifdef CONFIG_USE_CUSTOM_OTA
        if (count >= 5) {
          auto &app = Application::GetInstance();
          app.StartCheckNewVersionForCustom();
        } else
#endif
            if (count >= 3) {
          ResetWifiConfiguration();
        }
      });

      if (POWER_KEEP_GPIO != GPIO_NUM_NC) {
        boot_button_->OnLongPress(
            [this]() { gpio_set_level(POWER_KEEP_GPIO, POWER_KEEP_LEVEL?0:1); });
      }
    }

#if defined(CONFIG_USE_DEVICE_AEC)
    if (volume_up_button_ && volume_down_button_) {
      volume_up_button_->OnPressDown([this]() {
        volume_up_press = 1;
        if (volume_up_press == 1 && volume_down_press == 1) {
          volume_up_down_press = 1;
        }
      });
      volume_up_button_->OnPressUp([this]() { volume_up_press = 2; });

      volume_down_button_->OnPressDown([this]() {
        volume_down_press = 1;
        if (volume_up_press == 1 && volume_down_press == 1) {
          volume_up_down_press = 1;
        }
      });
      volume_down_button_->OnPressUp([this]() { volume_down_press = 2; });
    }
#endif

    if (volume_up_button_) {
      volume_up_button_->OnClick([this]() {
#if defined(CONFIG_USE_DEVICE_AEC)
        if (volume_up_button_ && volume_down_button_) {
          if (volume_up_down_press) {
            if (volume_up_press == 2 && volume_down_press == 2) {
              ToggleRealtimeChatMode();
            }
            volume_up_press = 0;
            if (volume_up_press == 0 && volume_down_press == 0) {
              volume_up_down_press = 0;
            }
            return;
          }
        }
#endif
        // if (power_save_timer_) {
        //   power_save_timer_->WakeUp();
        // }
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + 10;
        if (volume > 100) {
          volume = 100;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                       std::to_string(volume));
      });

      volume_up_button_->OnLongPress([this]() {
        // if (power_save_timer_) {
        //   power_save_timer_->WakeUp();
        // }
        GetAudioCodec()->SetOutputVolume(100);
        GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
      });
    }
    if (volume_down_button_) {
      volume_down_button_->OnClick([this]() {
#if defined(CONFIG_USE_DEVICE_AEC)
        if (volume_up_button_ && volume_down_button_) {
          if (volume_up_down_press) {
            if (volume_up_press == 2 && volume_down_press == 2) {
              ToggleRealtimeChatMode();
            }
            volume_down_press = 0;
            if (volume_up_press == 0 && volume_down_press == 0) {
              volume_up_down_press = 0;
            }
            return;
          }
        }
#endif
        // if (power_save_timer_) {
        //   power_save_timer_->WakeUp();
        // }
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() - 10;
        if (volume < 0) {
          volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                       std::to_string(volume));
      });

      volume_down_button_->OnLongPress([this]() {
        // if (power_save_timer_) {
        //   power_save_timer_->WakeUp();
        // }
        GetAudioCodec()->SetOutputVolume(0);
        GetDisplay()->ShowNotification(Lang::Strings::MUTED);
      });
    }
  }

public:
  Mz01C3Lcd() {
    if (BOOT_BUTTON_GPIO != -1) {
      boot_button_ = new Button(BOOT_BUTTON_GPIO, false, 3000);
    }
    if (VOLUME_UP_BUTTON_GPIO != -1) {
      volume_up_button_ = new Button(VOLUME_UP_BUTTON_GPIO, false, 3000);
    }
    if (VOLUME_DOWN_BUTTON_GPIO != -1) {
      volume_down_button_ = new Button(VOLUME_DOWN_BUTTON_GPIO, false, 3000);
    }
    audio_codec_ = new vbAudioCodec(CODEC_TX_GPIO, CODEC_RX_GPIO);
    led_ = new SingleLed(RGB_DI_GPIO);

#if defined(CONFIG_USE_DEVICE_AEC)
    Settings settings("realtime", false);
#ifdef CONFIG_DEVICE_AEC_DEFAULT_OFF
    bool realtime_chat_enabled = settings.GetInt("enable", false);
#else
    bool realtime_chat_enabled = settings.GetInt("enable", true);
#endif
    auto &app = Application::GetInstance();
    if (realtime_chat_enabled) {
      app.SetAecMode(kAecOnDeviceSide, false);
    } else {
      app.SetAecMode(kAecOff, false);
    }
// #else
//     if (GetBoardCfg()->GetFunEnable(kFunEnableDeviceAce)) {
//       ESP_LOGW(TAG, "CONFIG_USE_DEVICE_AEC not enable");
//     }
#endif

    gpio_set_direction(POWER_KEEP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(POWER_KEEP_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_level(POWER_KEEP_GPIO, POWER_KEEP_LEVEL);

    McpAlarm::GetInstance()->Init();
    McpOtaChecker::GetInstance()->Init();
    McpPowerManager::GetInstance()->Init();
    McpMusic::Init(audio_codec_);

    InitializePowerManager();

    InitializePowerSaveTimer();

    InitializeButtons();
    vb_event_register(
        VB_EVT_WAKE_WORD,
        [](uint32_t, void *data, uint16_t len, void*) {
          const char* expected = vb_adapter_get_wake_word();
          if (!data || !len || !expected) {
            return;
          }
          const char* word = static_cast<const char*>(data);
          while (len && word[len - 1] == '\0') {
            --len;
          }
          if (std::string_view(word, len) != expected) {
            return;
          }
          ESP_LOGI(TAG, "wake word detected: pausing 701 music first");
#ifdef CONFIG_LANGUAGE_ZH_CN
          McpMusic::WakeWord("你好");
#else
          McpMusic::WakeWord("");
#endif
        },
        this);

    if (wifi_config_mode_ == false &&
        !SsidManager::GetInstance().GetSsidList().empty()) {
#ifdef CONFIG_POWER_ON_SOUNDS
      if (boot_sound_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void *arg) {
                  auto this_ = (Mz01C3Lcd *)arg;
                  esp_reset_reason_t reason = esp_reset_reason();
                  if (reason == ESP_RST_POWERON ||
                      reason == ESP_RST_DEEPSLEEP) {
                    Application::GetInstance().PlaySound(
                        Lang::Sounds::OGG_POWER_ON);
                  }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "boot_sound_timer",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &boot_sound_timer_);
      }
      if (boot_sound_timer_) {
        esp_timer_start_once(boot_sound_timer_, 150 * 1000);
      }
#endif

#ifdef CONFIG_NET_STA_SOUNDS
      esp_event_handler_register(
          WIFI_EVENT, ESP_EVENT_ANY_ID,
          [](void *event_handler_arg, esp_event_base_t event_base,
             int32_t event_id, void *event_data) {
            static bool conn_status = false;
            if (event_id == WIFI_EVENT_STA_CONNECTED) {
              if (conn_status == false) {
                conn_status = true;
                Application::GetInstance().PlaySound(
                    Lang::Sounds::OGG_NET_CONNECT);
              }
            } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
              if (conn_status == true) {
                conn_status = false;
                Application::GetInstance().PlaySound(
                    Lang::Sounds::OGG_NET_DISCONNECT);
              }
            }
          },
          NULL);
#endif
    }
  }

  virtual AudioCodec *GetAudioCodec() override {
    return (AudioCodec *)audio_codec_;
  }

  virtual Display *GetDisplay() override {
    static NoDisplay display;
    return &display;
  }

  virtual bool GetBatteryLevel(int &level, bool &charging,
                               bool &discharging) override {
    if (power_manager_) {
      static bool last_discharging = false;
      charging = power_manager_->IsCharging();
      discharging = power_manager_->IsDischarging();
      if (discharging != last_discharging) {
        // if (power_save_timer_) {
        //   power_save_timer_->SetEnabled(discharging);
        // }
        last_discharging = discharging;
      }
      level = power_manager_->GetBatteryLevel();
      return true;
    } else {
      return Board::GetBatteryLevel(level, charging, discharging);
    }
  }

  // virtual void SetPowerSaveMode(bool enabled) override {
  //   if (!enabled && power_save_timer_) {
  //     power_save_timer_->WakeUp();
  //   }
  //   WifiBoard::SetPowerSaveMode(enabled);
  // }

  virtual Led* GetLed() override {
    return led_;
  }
};
