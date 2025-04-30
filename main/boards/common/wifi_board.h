#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

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

    // 新增：保存BLE配网过程中收到的SSID/密码
    std::string ble_ssid_;
    std::string ble_password_;

public:
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual Http* CreateHttp() override;
    virtual WebSocket* CreateWebSocket() override;
    virtual Mqtt* CreateMqtt() override;
    virtual Udp* CreateUdp() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
};

#endif // WIFI_BOARD_H
