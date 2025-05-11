#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // 添加 FreeRTOS 任务头文件

// ble-WiFi 连接失败原因枚举
enum ble_wifi_reason_t { // 改名为 ble_wifi_reason_t
    BLE_WIFI_REASON_AUTH_FAIL = 1,        // 认证失败（密码错误）
    BLE_WIFI_REASON_NO_AP_FOUND = 2,      // 未找到AP（SSID不存在）
    BLE_WIFI_REASON_CONNECTION_FAIL = 3,   // 连接失败（其他原因）
    BLE_WIFI_REASON_INIT_FAIL = 4,     // WiFi初始化失败
};

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;

    WifiBoard();
    void EnterWifiConfigMode();
    virtual std::string GetBoardJson() override;

    // 新增：BLE配网流程中的WiFi连接
    void ConnectWifiByBle(const std::string& ssid, const std::string& password);
    
    // 新增：尝试连接已保存的WiFi网络
    bool TryConnectSavedWifi();
    
    // 新增：启动配网模式
    void StartConfigMode();

    // 新增：保存BLE配网过程中收到的SSID/密码
    std::string ble_ssid_;
    std::string ble_password_;

    // 新增：配网超时任务句柄
    TaskHandle_t wifi_timeout_task_handle_ = nullptr;
    // 新增：配网超时时间（分钟）
    int config_timeout_minutes_ = 3; // 默认3分钟

private:
    // 新增：重构 EnterWifiConfigMode 的辅助函数
    bool SetupBleCallbacks();                   // 设置BLE回调
    bool InitializeAndStartBleAdvertising();    // 初始化并开始BLE广播
    void UpdateUiForBleConfig();                // 更新UI以显示BLE配网状态
    bool StartWifiConfigTimeoutTask();          // 启动配网超时任务

public:
    virtual std::string GetBoardType() override;    // 新增：返回板卡类型
    virtual void StartNetwork() override;           // 新增：开始网络
    virtual Http* CreateHttp() override;            // 新增：创建HTTP对象
    virtual WebSocket* CreateWebSocket() override;  // 新增：创建WebSocket对象
    virtual Mqtt* CreateMqtt() override;            // 新增：创建MQTT对象
    virtual Udp* CreateUdp() override;              // 新增：创建UDP对象
    virtual const char* GetNetworkStateIcon() override; // 新增：获取网络状态图标
    virtual void SetPowerSaveMode(bool enabled) override;   // 新增：设置省电模式
    virtual void ResetWifiConfiguration();            // 新增：重置WiFi配置
    int GetConfigTimeoutMinutes() const { return config_timeout_minutes_; } // 新增：获取配网超时时间
    void ResetTimeoutTaskHandle() { wifi_timeout_task_handle_ = nullptr; } // 新增：重置超时任务句柄
};

#endif // WIFI_BOARD_H
