#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_wifi_types_generic.h>

#include "esp_netif.h"

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

class WifiStation {
public:
    static WifiStation& GetInstance();
    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveMode(bool enabled);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnScanBegin(std::function<void()> on_scan_begin);


    
// protected:

    // 这些变量原本在 private: 下面
    esp_netif_t *default_netif_ = nullptr; // 让我们可以检查和销毁它
    bool started_ = false;                 // 让我们可以读写启动状态
    bool connected_ = false;               // 让我们可以读写连接状态
    std::string ip_address_;               // 让我们可以读写IP地址
    std::vector<std::pair<std::string, std::string>> auths_; // 让我们可以访问认证信息
    // on_connect_, on_connected_, on_scan_begin_ 这些回调函数指针保持 private 可能没问题，
    // 因为它们是通过 public 的 OnXXX 方法设置的。
    // 事件处理器句柄 instance_any_id_, instance_got_ip_ 也建议移到 protected 或 public，方便 Stop 时注销
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;



private:
    WifiStation();
    ~WifiStation();
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    // esp_event_handler_instance_t instance_any_id_ = nullptr;
    // esp_event_handler_instance_t instance_got_ip_ = nullptr;
    std::string ssid_;
    std::string password_;
    // std::string ip_address_;
    int reconnect_count_ = 0;
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void()> on_scan_begin_;
    std::vector<WifiApRecord> connect_queue_;

    void HandleScanResult();
    void StartConnect();
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
