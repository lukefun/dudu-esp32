#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

// C++标准库头文件
#include <string>        // 字符串处理库，提供std::string类
#include <functional>    // 函数对象库，提供std::function等功能
#include <atomic>        // 原子操作库，提供线程安全的原子类型
#include <mutex>         // 互斥锁库，提供std::mutex等线程同步机制

// C标准库头文件
#include <stdint.h>      // 固定宽度整数类型定义，如uint16_t等

// FreeRTOS实时操作系统头文件
#include "freertos/FreeRTOS.h"      // FreeRTOS核心功能
#include "freertos/task.h"          // 任务管理功能，提供TaskHandle_t等
#include "freertos/event_groups.h"  // 事件组功能，用于任务间通信

// ESP-IDF框架相关头文件
#include "esp_event.h"              // ESP-IDF事件循环库，用于系统事件管理
#include "esp_log.h"                // ESP-IDF日志库，用于调试日志输出

// NimBLE蓝牙协议栈相关头文件
#include "host/ble_hs.h"            // NimBLE主机层核心功能，蓝牙基本操作
#include "nimble/nimble_port.h"     // NimBLE与ESP32平台适配层
#include "host/ble_uuid.h"          // NimBLE UUID处理功能，用于标识服务和特征
#include "host/ble_gap.h"           // NimBLE GAP层功能，通用访问配置文件

// 项目自定义头文件
#include "ble_task_state.h"         // 定义蓝牙任务状态枚举类型ble_task_state_t

// BLE服务和特征的UUID定义
// WiFi配置服务UUID - 唯一标识WiFi配置相关服务
#define WIFI_CONFIG_SERVICE_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD63"
// SSID特征UUID - 用于传输WiFi网络名称
#define SSID_CHAR_UUID                "CDB7950D-73F1-4D4D-8E47-C090502DBD64"
// 密码特征UUID - 用于传输WiFi网络密码
#define PASSWORD_CHAR_UUID            "CDB7950D-73F1-4D4D-8E47-C090502DBD65"
// 控制状态特征UUID - 用于传输和接收控制命令与状态
#define CONTROL_STATUS_CHAR_UUID      "CDB7950D-73F1-4D4D-8E47-C090502DBD66"

// BLE事件定义
// 声明一个基础事件类型，作为所有BLE相关事件的基类
ESP_EVENT_DECLARE_BASE(BLE_EVENT_BASE);

// BLE事件枚举
enum {
    BLE_EVENT_SHUTDOWN,     // 通知BLE任务关闭的事件
    BLE_EVENT_MAX           // BLE事件的最大数量，用于边界检查
};

// 事件组位定义
// 事件组中用于标记BLE关闭事件的位
#define BLE_SHUTDOWN_BIT (1 << 0)

// Wi-Fi配置状态码（通过BLE传输）
typedef enum {
    WIFI_STATUS_IDLE = 0x00,              // 空闲状态
    WIFI_STATUS_CONNECTING = 0x01,        // 正在尝试连接Wi-Fi
    WIFI_STATUS_CONNECTED = 0x02,         // 已成功连接到Wi-Fi
    WIFI_STATUS_FAIL = 0x03,              // 通用Wi-Fi连接失败
    WIFI_STATUS_WEAK_SIGNAL = 0x04,       // Wi-Fi信号强度太弱
    WIFI_STATUS_FAIL_AUTH = 0x05,         // Wi-Fi认证失败（如密码错误）
    WIFI_STATUS_FAIL_AP_NOT_FOUND = 0x06, // 未找到指定的Wi-Fi接入点
    WIFI_STATUS_FAIL_CONN = 0x07,         // 通用连接失败（如DHCP失败）
    WIFI_STATUS_FAIL_SSID = 0x08,         // SSID为空或无效
    WIFI_STATUS_FAIL_OTHER = 0x09         // 其他/未指定的Wi-Fi连接错误
} wifi_config_status_t;

// 控制命令码（通过BLE接收）
#define WIFI_CONTROL_CMD_CONNECT 0xFF // 启动Wi-Fi连接的命令

// BLE设备名称
constexpr const char* kBleDeviceName = "DuDu-BLE"; // 广播的BLE设备名称

// BLE操作模式
enum class BleOperationMode {
    NORMAL,         // 正常工作模式，允许自动重连和广播
    SHUTTING_DOWN,  // 正在关闭模式，不允许自动重连和广播
    DISABLED        // BLE完全禁用模式
};

/**
 * BLE配置管理类 - 单例模式实现
 * 负责管理BLE栈初始化、广播控制、Wi-Fi配置状态通信等功能
 */
class BleConfig {
    public:
        // 单例访问器 - 获取唯一实例
        static BleConfig& GetInstance();
    
        // 删除拷贝构造函数和赋值运算符，确保单例特性
        BleConfig(const BleConfig&) = delete;
        BleConfig& operator=(const BleConfig&) = delete;
    
        // 初始化和反初始化
        void Initialize();    // 初始化BLE配置并启动BLE主机任务
        void Deinitialize();  // 停止BLE操作并反初始化BLE栈
    
        // 广播控制
        bool StartAdvertising(); // 启动BLE广播
        bool StopAdvertising();  // 停止BLE广播
        bool IsAdvertising() const; // 检查当前是否正在广播

        // Wi-Fi状态和凭证处理
        void SendWifiStatus(wifi_config_status_t status);                                                                   // 向连接的BLE客户端发送Wi-Fi连接状态
        void SetCredentialsReceivedCallback(std::function<void(const std::string& ssid, const std::string& password)> cb);  // 设置Wi-Fi凭证接收回调
        void SetConnectWifiCallback(std::function<void()> cb);                                                              // 设置连接Wi-Fi的回调

        // 操作模式管理
        void SetOperationMode(BleOperationMode mode);
        BleOperationMode GetOperationMode();
        bool IsAutoAdvertisingAllowed(); // 检查基于当前模式是否允许自动广播

        // 工具方法
        void ForceReset(); // 强制重置BLE状态（谨慎使用）
        static TaskHandle_t GetBleHostTaskHandle(); // 获取BLE主机任务句柄

        // 公共成员 - 用于BLE任务和回调（如果可能应考虑封装）
        std::string received_ssid_;     // 存储接收到的Wi-Fi SSID
        std::string received_password_; // 存储接收到的Wi-Fi密码

        // BLE主机任务状态变量（公开以便任务和静态回调直接访问）
        // 如果需要更多控制，可以考虑使用访问器或友元类
        static volatile bool ble_host_task_running; // 指示BLE主机任务是否应运行的标志
        static volatile ble_task_state_t ble_host_task_state; // BLE主机任务的当前状态

    private:
        // 私有构造函数和析构函数，实现单例模式
        BleConfig();             // 私有构造函数，防止外部实例化
        ~BleConfig();            // 私有析构函数，防止外部删除实例
    
        // 实例成员变量
        uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE; // 连接句柄，未连接时为BLE_HS_CONN_HANDLE_NONE
        uint16_t status_val_handle_ = 0;                // Wi-Fi状态特征的GATT句柄
        
        // 回调函数
        std::function<void(const std::string& ssid, const std::string& password)> credentials_received_cb_ = nullptr;
        std::function<void()> connect_wifi_cb_ = nullptr;
    
        // BLE栈和任务管理的静态成员
        static TaskHandle_t ble_host_task_handle;   // BLE主机任务句柄
        static BleOperationMode ble_operation_mode_; // 当前BLE操作模式
        static std::mutex ble_state_mutex_;          // 保护共享BLE状态的互斥锁
        static esp_event_loop_handle_t ble_event_loop; // BLE事件的事件循环句柄

    public:
        // --- NimBLE静态回调函数 --- 
        // 这些是NimBLE协议栈所需的回调函数，必须定义为静态成员

        /**
         * @brief GATT服务器特征访问回调函数
         * 当客户端读取或写入GATT特征时被调用
         * 
         * @param conn_handle 连接句柄，标识发起访问的客户端
         * @param attr_handle 属性句柄，标识被访问的GATT属性
         * @param ctxt 访问上下文，包含访问类型(读/写)和数据
         * @param arg 传递给回调的参数(通常是类实例指针)
         * @return int 状态码，0表示成功，负值表示错误
         */
        static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
            struct ble_gatt_access_ctxt *ctxt, void *arg);

        /**
        * @brief GATT服务器注册回调函数
        * 当GATT服务和特征注册完成时被调用
        * 
        * @param ctxt 注册上下文，包含注册结果和分配的句柄
        * @param arg 传递给回调的参数
        */
        static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);

        /**
        * @brief 初始化GATT服务器
        * 设置并注册自定义Wi-Fi配置服务和特征
        * 定义服务UUID、特征UUID及访问权限
        */
        static void gatt_svr_init(void);

        /**
        * @brief 启动BLE广播
        * 配置广播数据和扫描响应数据
        * 启动低功耗蓝牙广播流程
        * 
        * @return bool 成功返回true，失败返回false
        */
        static bool ble_advertise(void);

        /**
        * @brief BLE栈同步回调
        * 当BLE主机栈初始化完成并准备好使用时被调用
        * 通常用于启动广播或其他初始化后操作
        */
        static void ble_on_sync(void);

        /**
        * @brief BLE栈重置回调
        * 当BLE栈因错误或其他原因重置时被调用
        * 
        * @param reason 重置原因代码
        */
        static void ble_on_reset(int reason);

        /**
        * @brief BLE主机主任务函数
        * FreeRTOS任务，运行NimBLE事件处理循环
        * 处理BLE栈内部事件和消息
        * 
        * @param param 传递给任务的参数(通常为NULL)
        */
        static void ble_host_task(void *param);

        /**
        * @brief BLE GAP事件回调
        * 处理GAP层事件，如连接、断开连接、广告状态变化等
        * 
        * @param event GAP事件结构体，包含事件类型和相关数据
        * @param arg 传递给回调的参数
        * @return int 状态码，0表示成功
        */
        static int ble_gap_event(struct ble_gap_event *event, void *arg);
};

// 外部声明：用于BLE存储配置（例如：用于存储配对信息）
extern "C" void ble_store_config_init(void);

#endif // BLE_CONFIG_H



