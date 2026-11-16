// mcp_alarm.hpp
#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mcp_server.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <time.h>

#define TAG "McpAlarm"

struct Alarm {
    std::string name;
    int time;
};

class AlarmManager {
public:
    AlarmManager(){
        ESP_LOGI(TAG, "AlarmManager init");
        ring_flag = false;
        running_flag = false;

        // 建立一个时钟
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                AlarmManager* alarm_manager = (AlarmManager*)arg;
                alarm_manager->OnAlarm(); // 闹钟响了
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "alarm_timer"
        };
        esp_timer_create(&timer_args, &timer_);
        time_t now = time(NULL);

        ClearOverdueAlarm(now);

        Alarm *current_alarm_ = GetProximateAlarm(now);
        // 启动闹钟
        if(current_alarm_ != nullptr){
            int new_timer_time = current_alarm_->time - now;
            ESP_LOGI(TAG, "begin a alarm at %d", new_timer_time);
            esp_timer_start_once(timer_, new_timer_time * 1000000);
            running_flag = true;
        }
    }
    
    ~AlarmManager(){
        if(timer_ != nullptr){
            esp_timer_stop(timer_);
            esp_timer_delete(timer_);
        }
    }

    // 设置闹钟
    void SetAlarm(int seconde_from_now, std::string alarm_name){
        std::lock_guard<std::mutex> lock(mutex_);
        if(alarms_.size() >= 10){
            ESP_LOGE(TAG, "Too many alarms");
            return;
        }
        if(seconde_from_now <= 0){
            ESP_LOGE(TAG, "Invalid alarm time");
            return;
        }

        Alarm alarm; // 一个新的闹钟
        alarm.name = alarm_name;
        time_t now = time(NULL);
        alarm.time = now + seconde_from_now;
        alarms_.push_back(alarm);

        Alarm *alarm_first = GetProximateAlarm(now);
        ESP_LOGI(TAG, "Alarm %s set at %d, now first %d", alarm.name.c_str(), alarm.time, alarm_first->time);
        if(running_flag == true){
            esp_timer_stop(timer_);
        }

        running_flag = true;

        seconde_from_now = alarm_first->time - now;
        ESP_LOGI(TAG, "begin a alarm at %d", seconde_from_now);
        esp_timer_start_once(timer_, seconde_from_now * 1000000); // 当前一定有时钟, 所以不需要清除标志
    }
    
    // 获取闹钟列表状态
    std::string GetAlarmsStatus(){
        std::lock_guard<std::mutex> lock(mutex_);
        std::string status;
        for(size_t i = 0; i < alarms_.size(); ++i){
            status += alarms_[i].name + " at " + std::to_string(alarms_[i].time);
            if(i != alarms_.size() - 1){
                status += ", ";
            }
        }
        return status;
    }

    void ClearAll(){
        std::lock_guard<std::mutex> lock(mutex_);
        alarms_.clear();
        
        // 停止当前运行的定时器
        if(timer_ != nullptr && running_flag) {
            esp_timer_stop(timer_);
            running_flag = false;
        }
        
        ESP_LOGI(TAG, "All alarms cleared");
    }

    void OnAlarm(std::function<void(int time, const std::string& name)> callback){
        callback_ = callback;
    }

private:
    // 清除过时的闹钟
    void ClearOverdueAlarm(time_t now){
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it = alarms_.begin(); it != alarms_.end();){
            if(it->time <= now){
                it = alarms_.erase(it); // 删除过期的闹钟, 此时it指向下一个元素
            }else{
                it++;
            }
        }
    }

    // 获取从现在开始第一个响的闹钟
    Alarm *GetProximateAlarm(time_t now){
        Alarm *current_alarm_ = nullptr;
        for(auto& alarm : alarms_){
            if(alarm.time > now && (current_alarm_ == nullptr || alarm.time < current_alarm_->time)){
                current_alarm_ = &alarm; // 获取当前时间以后第一个发生的时钟句柄
            }
        }
        return current_alarm_;
    }
    
    // 闹钟响了的处理函数
    void OnAlarm(){
        ring_flag = true;
    
        // 遍历闹钟
        Alarm *alarm_first = nullptr;
        for(auto& alarm : alarms_){
            if(alarm.time <= time(NULL)){
                alarm_first = &alarm;
                break;
            }
        }

        callback_(alarm_first->time, alarm_first->name);

        // 处理一下相同时间的闹钟
        time_t now = time(NULL);
        ClearOverdueAlarm(now);

        Alarm *current_alarm_ = GetProximateAlarm(now);
        if(current_alarm_ != nullptr){
            int new_timer_time = current_alarm_->time - now;
            ESP_LOGI(TAG, "begin a alarm at %d", new_timer_time);
            esp_timer_start_once(timer_, new_timer_time * 1000000);
        }else{
            running_flag = false; // 没有闹钟了
            ESP_LOGI(TAG, "no alarm now");
        }
    }

    // 闹钟是不是响了的标志位
    bool IsRing(){ return ring_flag; };
    // 清除闹钟标志位
    void ClearRing(){ESP_LOGI("Alarm", "clear");ring_flag = false;};

    std::vector<Alarm> alarms_; // 闹钟列表
    std::mutex mutex_; // 互斥锁
    esp_timer_handle_t timer_; // 定时器

    std::atomic<bool> ring_flag{false}; 
    std::atomic<bool> running_flag{false};

    std::function<void(int time, const std::string& name)> callback_;
};

class McpAlarm {
  AlarmManager *alarm_manager;
  std::function<void(int time, const std::string &name)> on_alarm_cb_;
  esp_timer_handle_t alarm_play_timer = nullptr;
  int alarm_cnt = -1;
  bool alarm_playing = false;

  void InitAlarm() {
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void* arg) {
        McpAlarm *mcp_alarm = static_cast<McpAlarm *>(arg);
        ESP_LOGW(TAG, "esp_timer_cb: %d", mcp_alarm->alarm_cnt);
        auto &app = Application::GetInstance();
        mcp_alarm->alarm_cnt++;
        if (app.GetDeviceState() == kDeviceStateIdle &&
            mcp_alarm->alarm_cnt <= 5) {
          // 使用非阻塞方式播放声音
          if (!mcp_alarm->alarm_playing) {
            mcp_alarm->alarm_playing = true;
            app.Schedule([]() {
              auto &app = Application::GetInstance();
              auto codec = Board::GetInstance().GetAudioCodec();
              codec->EnableOutput(true);
              app.PlaySound(Lang::Sounds::OGG_ALARM_RING);
              // 播放完成后重置标志
              McpAlarm::GetInstance()->alarm_playing = false;
            });
          }
        } else {
          mcp_alarm->StopAlarm();
        }
    };
    timer_args.arg = this;
    timer_args.name = "alarm_play_timer";
    
    esp_timer_create(&timer_args, &alarm_play_timer);

    alarm_manager->OnAlarm([this](int time, const std::string &name) {
      ESP_LOGI(TAG, "OnAlarm: %d, alarm_name: %s", time, name.c_str());
      if (on_alarm_cb_) {
        on_alarm_cb_(time, name);
      }

      auto &app = Application::GetInstance();
      app.Close();

      app.Schedule([this, name]() {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) {
          StartAlarm(name);
        } else {
          app.Schedule([this, name]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
              StartAlarm(name);
            }
          });
        }
      });
    });
  }

public:
  McpAlarm() : alarm_playing(false) {}
  
  ~McpAlarm() {
    if (alarm_play_timer) {
      esp_timer_stop(alarm_play_timer);
      esp_timer_delete(alarm_play_timer);
    }
  }
  
  static McpAlarm *GetInstance() {
    static McpAlarm instance;
    return &instance;
  }

  void SetOnAlarmCallback(std::function<void(int time, const std::string &name)> callback) {
    on_alarm_cb_ = callback;
  }

  void Init() {
    alarm_manager = new AlarmManager();
    InitAlarm();

    McpServer::GetInstance().AddTool(
        "self.alarm.set_alarm",
        "Sets an alarm. Parameters: seconds_from_now "
        "(int), alarm_name (string)",
        PropertyList({Property("seconds_from_now", kPropertyTypeInteger),
                      Property("alarm_name", kPropertyTypeString)}),
        [this](const PropertyList &properties) -> ReturnValue {
          int seconds_from_now = properties["seconds_from_now"].value<int>();
          std::string alarm_name =
              properties["alarm_name"].value<std::string>();
          alarm_manager->SetAlarm(seconds_from_now, alarm_name);
          return true;
        });
        
    McpServer::GetInstance().AddTool(
        "self.alarm.get_alarms_status",
        "Gets the current status of all alarms.", PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue {
          std::string status = alarm_manager->GetAlarmsStatus();
          ESP_LOGI(TAG, "Current alarms status: %s", status.c_str());
          return status;
        });
        
    // 添加清除所有闹钟的工具
    McpServer::GetInstance().AddTool(
        "self.alarm.clear_all_alarms",
        "Clears all alarms.",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue {
          alarm_manager->ClearAll();
          ESP_LOGI(TAG, "All alarms cleared via MCP command");
          return true;
        });
  }

  bool IsAlarm(void) { return (alarm_cnt > -1); }
  
  void StartAlarm(const std::string &name) {
    ESP_LOGW(TAG, "StartAlarm");
    alarm_cnt = 0;
    alarm_playing = false; // 重置播放标志
    
    if (alarm_play_timer) {
      esp_timer_stop(alarm_play_timer);
      esp_timer_start_periodic(alarm_play_timer, 4500000); // 4500ms in microseconds
    }
  }

  void StopAlarm(void) {
    ESP_LOGW(TAG, "StopAlarm");
    if (alarm_cnt > -1) {
      alarm_cnt = -1;
      alarm_playing = false; // 重置播放标志
      if (alarm_play_timer) {
        esp_timer_stop(alarm_play_timer);
      }
      
      // 确保音频输出被禁用
      auto codec = Board::GetInstance().GetAudioCodec();
      if (codec) {
        codec->EnableOutput(false);
      }
    }
  }
  
  AlarmManager* GetAlarmManager() {
    return alarm_manager;
  }
};