#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <string>
#include <functional>
#include <stdint.h>  // 新增，保证uint16_t等类型可用
#include "host/ble_hs.h" // NimBLE 头文件
#include "nimble/nimble_port.h"
#include "host/ble_uuid.h"  // 确保ble_uuid_any_t定义可见
#include "host/ble_gap.h"   // 确保ble_gap_event_listener定义完整

// 定义我们之前设计的 UUID
#define WIFI_CONFIG_SERVICE_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD63"
#define SSID_CHAR_UUID                "CDB7950D-73F1-4D4D-8E47-C090502DBD64"
#define PASSWORD_CHAR_UUID            "CDB7950D-73F1-4D4D-8E47-C090502DBD65"
#define CONTROL_STATUS_CHAR_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD66"

// 定义配网状态码
typedef enum {
    WIFI_STATUS_IDLE = 0x00,
    WIFI_STATUS_CONNECTING = 0x01,
    WIFI_STATUS_SUCCESS = 0x02,
    WIFI_STATUS_FAIL_PWD = 0x03,
    WIFI_STATUS_FAIL_SSID = 0x04,
    WIFI_STATUS_FAIL_OTHER = 0x05,
} wifi_config_status_t;

// 定义控制命令码 (从手机接收)
#define WIFI_CONTROL_CMD_CONNECT 0xFF


class BleConfig {
public:
    // 使用单例模式方便全局访问
    static BleConfig& GetInstance() {
        static BleConfig instance;
        return instance;
    }

    // 禁止拷贝和赋值
    BleConfig(const BleConfig&) = delete;
    BleConfig& operator=(const BleConfig&) = delete;

    // 初始化 BLE 配网功能
    void Initialize();

    // 开始 BLE 广播 (当设备未配网时调用)
    void StartAdvertising();

    // 停止 BLE 广播 (配网成功或不需要时调用)
    void StopAdvertising();

    // 发送 Wi-Fi 连接状态给手机
    void SendWifiStatus(wifi_config_status_t status);

    // 设置 Wi-Fi 凭据接收成功后的回调函数
    void SetCredentialsReceivedCallback(std::function<void(const std::string& ssid, const std::string& password)> cb);

    // 设置开始连接 Wi-Fi 的回调函数
    void SetConnectWifiCallback(std::function<void()> cb);

    // --- 公开给静态回调函数访问的成员 ---
    std::string received_ssid_;
    std::string received_password_;
    uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    uint16_t status_val_handle_ = 0; // Status Characteristic Value Handle
    std::function<void(const std::string& ssid, const std::string& password)> credentials_received_cb_ = nullptr;
    std::function<void()> connect_wifi_cb_ = nullptr;

    // --- 静态回调函数 ---
    static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);
    static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
    static void gatt_svr_init(void);
    static void ble_advertise(void);
    static void ble_on_sync(void);
    static void ble_on_reset(int reason);
    static void ble_host_task(void *param);
    static int ble_gap_event(struct ble_gap_event *event, void *arg);


private:
    BleConfig() = default;
    ~BleConfig() = default;
};

// 声明外部存储回调函数（如果需要持久化绑定信息）
 extern "C" void ble_store_config_init(void);

#endif // BLE_CONFIG_H
