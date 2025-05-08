#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <string>
#include <functional>
#include <stdint.h>  // 新增，保证uint16_t等类型可用
#include "host/ble_hs.h" // NimBLE 头文件
#include "nimble/nimble_port.h"
#include "host/ble_uuid.h"  // 确保ble_uuid_any_t定义可见
#include "host/ble_gap.h"   // 确保ble_gap_event_listener定义完整
#include "ble_task_state.h" // 包含任务状态定义

// 定义我们之前设计的 UUID
#define WIFI_CONFIG_SERVICE_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD63"
#define SSID_CHAR_UUID                "CDB7950D-73F1-4D4D-8E47-C090502DBD64"
#define PASSWORD_CHAR_UUID            "CDB7950D-73F1-4D4D-8E47-C090502DBD65"
#define CONTROL_STATUS_CHAR_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD66"

// 定义配网状态码
typedef enum {
    WIFI_STATUS_IDLE = 0x00,                // 新增，表示空闲状态
    WIFI_STATUS_CONNECTING = 0x01,          // 新增，表示正在连接状态
    WIFI_STATUS_CONNECTED = 0x02,           // 新增，表示已连接状态
    WIFI_STATUS_FAIL = 0x03,                // 新增，表示连接失败状态
    WIFI_STATUS_WEAK_SIGNAL = 0x04,         // 新增，表示信号强度过弱状态
    WIFI_STATUS_FAIL_AUTH = 0x05,           // 新增，表示认证失败状态
    WIFI_STATUS_FAIL_AP_NOT_FOUND = 0x06,   // 新增，表示AP未找到状态
    WIFI_STATUS_FAIL_CONN = 0x07,           // 新增，表示连接失败状态
    WIFI_STATUS_FAIL_SSID = 0x08,           // 新增，表示SSID为空状态
    WIFI_STATUS_FAIL_OTHER = 0x09           // 新增，表示其他错误状态
} wifi_config_status_t;

// 定义控制命令码 (从手机接收)
#define WIFI_CONTROL_CMD_CONNECT 0xFF

// BLE 设备名常量定义
constexpr const char* kBleDeviceName = "DuDu-BLE";

class BleConfig {
public:
    // 使用单例模式方便全局访问
    static BleConfig& GetInstance() {
        static BleConfig instance;
        return instance;
    }

    // 新增：获取BLE主机任务句柄
    // 修改为静态成员函数，返回静态成员变量
    static TaskHandle_t GetBleHostTaskHandle() { 
        return ble_host_task_handle; 
    }

    // 新增：完整去初始化BLE模块
    void Deinitialize();

    // 检查是否正在广播
    bool IsAdvertising() const {
        return ble_gap_adv_active();
    }

    // 禁止拷贝和赋值
    BleConfig(const BleConfig&) = delete;
    BleConfig& operator=(const BleConfig&) = delete;

    // 初始化 BLE 配网功能
    void Initialize();

    // 开始 BLE 广播 (当设备未配网时调用)
    bool StartAdvertising(); // 修改返回类型为 bool

    // 停止 BLE 广播 (配网成功或不需要时调用)
    void StopAdvertising();

    // 发送 Wi-Fi 连接状态给手机
    void SendWifiStatus(wifi_config_status_t status);

    // 设置 Wi-Fi 凭据接收成功后的回调函数
    void SetCredentialsReceivedCallback(std::function<void(const std::string& ssid, const std::string& password)> cb);

    // 设置开始连接 Wi-Fi 的回调函数
    void SetConnectWifiCallback(std::function<void()> cb);

    // --- 公开给静态回调函数访问的成员 ---
    // 用于存储接收到的 Wi-Fi 凭据
    std::string received_ssid_;
    // 用于存储接收到的 Wi-Fi 密码
    std::string received_password_;
    // --- 私有成员 ---
    // BLE_HS_CONN_HANDLE_NONE 表示没有连接
    uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    // 用于存储 BLE 服务句柄
    uint16_t status_val_handle_ = 0; // Status Characteristic Value Handle
    // 用于存储回调函数
    std::function<void(const std::string& ssid, const std::string& password)> credentials_received_cb_ = nullptr;
    // 用于存储开始连接 Wi-Fi 的回调函数
    std::function<void()> connect_wifi_cb_ = nullptr;

    // --- 静态回调函数 ---
    // 用于处理 GATT 事件
    static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);


    // 用于处理 GATT 服务注册事件
    /**
     * @brief 处理 GATT 服务器注册事件的回调函数
     *
     * 当 GATT 服务器进行服务、特征或描述符注册操作时，此回调函数会被调用。
     * 它根据注册上下文信息输出相应的注册结果，方便开发者了解注册状态。
     *
     * @param ctxt 指向包含注册操作详细信息的结构体指针
     * @param arg 传递给回调函数的额外参数，可用于传递自定义数据，这里未使用
     */
    static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);

    // 回调函数 - GATT 服务器初始化
    /**
     * @brief 初始化 GATT 服务器
     *
     * 此函数负责完成 GATT 服务器的初始化工作，例如注册服务、特征和描述符等。
     * 通常在蓝牙栈初始化完成后调用，以确保 GATT 服务器能正常工作。
     */
    static void gatt_svr_init(void);

    // 回调函数 - BLE 低功耗蓝牙广播
    /**
     * @brief 启动 BLE 低功耗蓝牙广播
     *
     * 该函数用于配置并启动 BLE 设备的广播功能，使设备能够被其他设备发现。
     * 它设置广播参数和广播数据，然后开始广播操作。
     */
    static void ble_advertise(void);
    
    // 回调函数 - BLE开启同步
    /**
     * @brief BLE 开启同步时的回调函数
     *
     * 当 BLE 设备与蓝牙协议栈同步完成后，此回调函数会被触发。
     * 一般在该函数中会调用 GATT 服务器初始化和广播启动函数，使设备进入可被发现和连接的状态。
     */
    static void ble_on_sync(void);

    // 回调函数 - 
    /**
     * @brief BLE 栈复位时的回调函数
     *
     * 当蓝牙协议栈发生复位操作时，此回调函数会被调用。
     * 它接收一个表示复位原因的整数参数，可用于记录复位信息或进行相应的错误处理。
     *
     * @param reason 表示蓝牙栈复位的原因，不同的数值代表不同的复位原因
     */
    static void ble_on_reset(int reason);

    /**
     * @brief BLE 主机任务函数
     *
     * 这是一个任务函数，通常作为一个独立的线程或任务运行。
     * 它负责初始化蓝牙栈，设置复位和同步回调函数，然后进入一个无限循环来处理蓝牙事件。
     *
     * @param param 传递给任务函数的参数，可用于传递自定义数据，这里未使用
     */
    static void ble_host_task(void *param);
    
    /**
     * @brief 处理 BLE GAP 事件的回调函数
     *
     * 当 BLE 设备的通用访问配置文件（GAP）发生事件时，此回调函数会被调用。
     * 它根据事件类型进行相应的处理，如处理连接、断开连接、广播完成等事件。
     *
     * @param event 指向包含 GAP 事件详细信息的结构体指针
     * @param arg 传递给回调函数的额外参数，可用于传递自定义数据，这里未使用
     * @return 处理结果，通常 0 表示处理成功，非 0 表示处理失败
     */
    static int ble_gap_event(struct ble_gap_event *event, void *arg);


private:
    BleConfig() = default;
    ~BleConfig() = default;

    // 任务状态管理
    static TaskHandle_t ble_host_task_handle;    // 用于存储任务句柄，
    public:
        // BLE主机任务运行标志和状态（需为public以便任务函数访问）
        static volatile bool ble_host_task_running;  // 用于指示任务是否正在运行
        static volatile ble_task_state_t ble_host_task_state;   // 用于存储任务状态

};

// 声明外部存储回调函数（如果需要持久化绑定信息）
 extern "C" void ble_store_config_init(void);

#endif // BLE_CONFIG_H


