// mcp_power_manager.hpp
#include "application.h"
#include "esp_log.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "McpPowerManager"

class McpPowerManager {
public:
  McpPowerManager() {}
  
  ~McpPowerManager() {}
  
  static McpPowerManager *GetInstance() {
    static McpPowerManager instance;
    return &instance;
  }

  void Init() {
    McpServer::GetInstance().AddTool(
        "self.low_power.set_auto_poweroff",
        "Sets whether to enable automatic power off. Parameters: enable "
        "(true/false)",
        PropertyList({Property("enable", kPropertyTypeBoolean)}),
        [](const PropertyList &properties) -> ReturnValue {
          bool enable = properties["enable"].value<bool>();
          Settings settings("low_power", true);
          settings.SetInt("auto_poweroff", enable ? 1 : 0);
          return true;
        });
        
    McpServer::GetInstance().AddTool(
        "self.low_power.get_auto_poweroff",
        "Gets whether automatic power off is enabled.", PropertyList(),
        [](const PropertyList &properties) -> ReturnValue {
          Settings settings("low_power", false);
          auto auto_poweroff_ =
              settings.GetInt("auto_poweroff", 1) ? true : false;
          ESP_LOGI(TAG, "Auto power off is %s",
                   auto_poweroff_ ? "enabled" : "disabled");
          return auto_poweroff_;
        });
  }

  bool GetIsAutoPoweroff() {
    Settings settings("low_power", false);
    auto auto_poweroff_ = settings.GetInt("auto_poweroff", 1) ? true : false;
    ESP_LOGI(TAG, "Auto power off is %s",
             auto_poweroff_ ? "enabled" : "disabled");
    return auto_poweroff_;
  }
};