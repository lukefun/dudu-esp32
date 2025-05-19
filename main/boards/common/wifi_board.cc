#include "wifi_board.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_log.h>
#include <esp_mqtt.h>
#include <esp_task_wdt.h>
#include <esp_udp.h>
#include <sys/time.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <inttypes.h>   // <--- 包含 C++11 标准库头文件

#include "application.h"        // <--- 包含 Application 头文件
#include "assets/lang_config.h" // <--- 包含语言配置头文件
#include "ble_config/ble_config.h"
#include "board.h" // <--- 包含 Board 头文件
#include "display.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_configuration_ap.h"
#include "wifi_station.h"

static const char *TAG = "WifiBoard"; // <--- 确保 TAG 已定义

// Forward declaration for timeout task
static void WifiConfigTimeoutTask(void *pvParameters);



// RAII 风格的看门狗管理类，通过构造函数注册看门狗，析构函数自动注销看门狗
class WdtGuard
{
public:
    WdtGuard()
    {
        // 先检查任务是否已订阅看门狗，避免重复添加导致错误
        esp_err_t err = esp_task_wdt_add(NULL);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "%s @WdtGuard：看门狗已注册", GetTimeString().c_str());
        }
        else if (err == ESP_ERR_INVALID_ARG)
        {
            // 任务已经订阅了看门狗，记录日志但不报错
            ESP_LOGW(TAG, "%s @WdtGuard：任务已订阅看门狗，跳过注册", GetTimeString().c_str());
        }
        else
        {
            // 其他错误则使用 ESP_ERROR_CHECK 处理
            ESP_ERROR_CHECK(err);
            ESP_LOGE(TAG, "@WdtGuard：看门狗注册失败，错误码: %d", err);
        }
    }

    ~WdtGuard()
    {
        esp_task_wdt_delete(NULL); // 自动注销看门狗
        ESP_LOGI(TAG, "%s @WdtGuard：看门狗已注销", GetTimeString().c_str());
    }

    // 禁止拷贝构造和赋值操作，确保资源唯一性
    WdtGuard(const WdtGuard &) = delete;
    WdtGuard &operator=(const WdtGuard &) = delete;
};

WifiBoard::WifiBoard()
{
    ESP_LOGI(TAG, "%s @WifiBoard：初始化 WifiBoard", GetTimeString().c_str());
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_)
    {
        ESP_LOGI(TAG, "%s @WifiBoard：检测到强制配网标志 force_ap=1，重置为0", GetTimeString().c_str());
        settings.SetInt("force_ap", 0);
    }
    ESP_LOGI(TAG, "%s @WifiBoard：WifiBoard 初始化完成，配网模式状态: %s", GetTimeString().c_str(), wifi_config_mode_ ? "启用" : "禁用");
}

std::string WifiBoard::GetBoardType()
{
    ESP_LOGD(TAG, "@GetBoardType：获取板卡类型: wifi");
    return "wifi";
}

// --- 重构 EnterWifiConfigMode 的辅助函数实现 ---
// 设置 BLE 回调函数
bool WifiBoard::SetupBleCallbacks()
{
    ESP_LOGI(TAG, "%s @SetupBleCallbacks：设置 BLE 回调函数", GetTimeString().c_str());
    auto &ble_config = BleConfig::GetInstance();

    // 1. 设置 WiFi 凭据接收回调
    ble_config.SetCredentialsReceivedCallback([this](const std::string &ssid, const std::string &password)
    {
        std::lock_guard<std::mutex> lock(ble_config_mutex_);
        
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.CredentialsReceivedCallback：收到 WiFi 凭据 - SSID: %s", 
                 GetTimeString().c_str(), ssid.c_str());
        
        // 只在ADVERTISING或CONNECTED状态接收凭据
        if (ble_config_state_ == BleConfigState::ADVERTISING || 
            ble_config_state_ == BleConfigState::CONNECTED) {
            ble_ssid_ = ssid;
            ble_password_ = password;
            ble_config_state_ = BleConfigState::CREDENTIALS_RECEIVED;
            ESP_LOGI(TAG, "%s @SetupBleCallbacks.CredentialsReceivedCallback：WiFi 凭据已暂存，状态更新为CREDENTIALS_RECEIVED", 
                     GetTimeString().c_str());
        } else {
            ESP_LOGW(TAG, "%s @SetupBleCallbacks.CredentialsReceivedCallback：当前状态(%d)不适合接收凭据，已忽略", 
                     GetTimeString().c_str(), static_cast<int>(ble_config_state_));
        }
    });

    // 2. 设置开始连接 WiFi 的回调
    ble_config.SetConnectWifiCallback([this]()
    {
        std::lock_guard<std::mutex> lock(ble_config_mutex_);
        
        // 只在CREDENTIALS_RECEIVED状态处理连接命令
        if (ble_config_state_ != BleConfigState::CREDENTIALS_RECEIVED) {
            ESP_LOGW(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：当前状态(%d)不是CREDENTIALS_RECEIVED，忽略连接命令", 
                     GetTimeString().c_str(), static_cast<int>(ble_config_state_));
            return;
        }
        
        // 更新状态为正在连接WiFi
        ble_config_state_ = BleConfigState::CONNECTING_WIFI;
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：状态更新为CONNECTING_WIFI", GetTimeString().c_str());
        
        // 设置标志让主循环处理WiFi连接
        should_connect_wifi_ = true;
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：已设置连接标志", GetTimeString().c_str());
    });

    return true;
}

bool WifiBoard::InitializeAndStartBleAdvertising()
{
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：初始化并启动 BLE 广播", GetTimeString().c_str());
    auto &ble_config = BleConfig::GetInstance(); // 获取 ble_config 实例
    // auto& application = Application::GetInstance();    // 获取 application 实例 (已注释掉，因为后续代码未使用)

    // 重置看门狗，防止初始化过程中触发超时
    esp_task_wdt_reset();

    // 检查内存并采取保守策略：如果内存不足，先进行堆检查
    MemorySnapshot memory_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "InitializeAndStartBleAdvertising：初始化前", memory_snapshot);

    if (memory_snapshot.internal_ram < 60000)
    {
        ESP_LOGW(TAG, "%s @InitializeAndStartBleAdvertising：内存较低 (%d字节)，采用保守策略", GetTimeString().c_str(), (int)memory_snapshot.internal_ram);

        esp_task_wdt_reset();                // 重置看门狗，防止堆检查过程中触发超时
        heap_caps_check_integrity_all(true); // 进行堆检查
        // 当设置为 true 时，如果该函数检测到任何堆损坏 (Heap Corruption)，它会 立即调用 abort() 函数，
        // 直接让程序中止执行 。它不会返回错误代码让您的程序去判断和处理，而是直接停止。
        // 这是为了确保在内存不足的情况下，程序不会继续执行可能导致不可预测的后果的代码。
        esp_task_wdt_reset(); // 重置看门狗
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_task_wdt_reset(); // 重置看门狗
    }

    // 初始化 BLE
    // 获取当前内存快照
    MemorySnapshot snapshot = get_memory_snapshot();
    log_memory_state(TAG, "InitializeAndStartBleAdvertising：初始化 BLE 前", snapshot);

    esp_task_wdt_reset();    // 重置看门狗，BLE初始化前
    ble_config.Initialize(); // 初始化 BLE - 这是一个耗时操作
    esp_task_wdt_reset();    // 重置看门狗，BLE初始化后

    snapshot = get_memory_snapshot();
    log_memory_state(TAG, "InitializeAndStartBleAdvertising：初始化 BLE 后", snapshot);

    esp_task_wdt_reset();           // 重置看门狗，延迟前
    vTaskDelay(pdMS_TO_TICKS(300)); // 短暂延迟，确保 BLE 初始化完成
    esp_task_wdt_reset();           // 重置看门狗，延迟后

    return true; // 返回成功
}

// 更新 UI 以显示 BLE 配网状态
void WifiBoard::UpdateUiForBleConfig()
{
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：更新 UI 及播放提示音", GetTimeString().c_str());
    auto &application = Application::GetInstance();

    // 优化提示合成，合并为一条 Alert，BLE 设备名可灵活配置
    std::string ble_hint;
    if (Lang::Strings::CONNECT_TO_BLE && Lang::Strings::CONNECT_TO_BLE[0] != '\0')
    {
        ble_hint = Lang::Strings::CONNECT_TO_BLE;
    }
    else
    {
        ESP_LOGW(TAG, "@UpdateUiForBleConfig：未找到 CONNECT_TO_BLE 字符串");
        ble_hint = "请使用支持BLE的手机App扫描并连接设备：";
    }
    // 合成最终提示
    ble_hint += std::string(" ") + kBleDeviceName;
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：配网提示: %s", GetTimeString().c_str(), ble_hint.c_str());


    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig： ------------------------------------------------------------ ", GetTimeString().c_str()); // 分割线
    application.Alert(Lang::Strings::BLE_CONFIG_MODE, ble_hint.c_str(), "thinking", Lang::Sounds::P3_WIFICONFIG);                       // 显示提示,播放提示音
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig： ------------------------------------------------------------ ", GetTimeString().c_str()); // 分割线

    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：显示 BLE 配网提示 和 播放提示音：\"进入配网模式\"完成", GetTimeString().c_str());
}

/**
 * @brief 启动Wi-Fi配网超时监控任务。
 *
 * 此函数负责创建一个新的超时任务 (WifiConfigTimeoutTask)。
 * 在创建新任务之前，它会检查是否存在一个旧的、可能仍在运行的超时任务句柄。
 * 如果存在旧句柄，会尝试删除该旧任务，以防止任务重复或资源泄漏。
 * 新创建的超时任务会在指定时间（config_timeout_minutes_）后触发一次语音提示，
 * 然后自行结束。
 *
 * @return true 如果超时任务成功创建。
 * @return false 如果超时任务创建失败。
 */
bool WifiBoard::StartWifiConfigTimeoutTask()
{
    ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：准备启动配网超时监控任务。", GetTimeString().c_str());

    // 步骤 1: 清理可能存在的旧超时任务句柄
    // 这是为了确保在创建新任务前，任何之前未正确结束的同类任务被处理。
    if (wifi_timeout_task_handle_ != nullptr)
    {
        ESP_LOGW(TAG, "%s @StartWifiConfigTimeoutTask：检测到存在旧的超时任务句柄 (%p)，尝试删除该任务。",
                 GetTimeString().c_str(), wifi_timeout_task_handle_);

        // 尝试删除任务。vTaskDelete可以安全地传入一个可能已经结束的任务的句柄。
        // 如果任务已经自我删除了，vTaskDelete不会产生错误。
        vTaskDelete(wifi_timeout_task_handle_);

        // 将句柄置空，表示旧任务已被处理或不再追踪。
        wifi_timeout_task_handle_ = nullptr;
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：旧超时任务句柄已清理。", GetTimeString().c_str());
    }
    else
    {
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：未检测到旧的超时任务句柄，直接创建新任务。", GetTimeString().c_str());
    }

    // 步骤 2: 创建新的超时任务 (WifiConfigTimeoutTask)
    ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：创建新的配网超时任务，超时时间: %d 分钟。",
             GetTimeString().c_str(), config_timeout_minutes_);

    // xTaskCreate 参数说明:
    // pvTaskCode: WifiConfigTimeoutTask (任务函数指针)
    // pcName: "WifiTimeoutTask" (任务名，用于调试)
    // usStackDepth: 4096 (任务栈大小，根据实际需求调整)
    // pvParameters: this (传递给任务函数的参数，即当前 WifiBoard 实例的指针)
    // uxPriority: 5 (任务优先级，根据系统设计调整)
    // pxCreatedTask: &wifi_timeout_task_handle_ (用于接收新创建任务的句柄)
    BaseType_t result = xTaskCreate(
        WifiConfigTimeoutTask,     // 任务的执行函数
        "WifiTimeoutTask",         // 任务名称，便于调试
        4096,                      // 任务堆栈大小 (字节)
        this,                      // 传递给任务函数的参数 (指向当前WifiBoard对象)
        5,                         // 任务优先级 (0是最低)
        &wifi_timeout_task_handle_ // 用于存储新创建任务的句柄
    );

    // 步骤 3: 检查任务创建结果并返回
    if (result != pdPASS)
    {
        // 如果任务创建失败
        ESP_LOGE(TAG, "@StartWifiConfigTimeoutTask：创建配网超时任务失败，xTaskCreate 返回错误码: %d", result);
        // 确保在失败时，任务句柄也被置为nullptr，避免悬空指针。
        wifi_timeout_task_handle_ = nullptr;
        return false; // 返回创建失败
    }

    // 任务创建成功
    ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：配网超时任务已成功创建，任务句柄: %p。",
             GetTimeString().c_str(), wifi_timeout_task_handle_);
    return true; // 返回创建成功
}

void WifiBoard::EnterWifiConfigMode()
{
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：进入 WiFi BLE 配网模式", GetTimeString().c_str());

    // --- 1. 准备阶段 ---
    {
        std::lock_guard<std::mutex> lock(ble_config_mutex_);
        ble_config_state_ = BleConfigState::IDLE;
    }
    
    // 获取当前内存快照
    MemorySnapshot snapshot = get_memory_snapshot();
    
    // 记录内存状态 (默认使用调试级别)
    log_memory_state(TAG, "@EnterWifiConfigMode：BLE 初始化前", snapshot);
    esp_task_wdt_reset(); // 重置看门狗

    auto &application = Application::GetInstance();
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：Application 实例获取成功", GetTimeString().c_str());

    // 设置设备状态为正在进行Wi-Fi配置
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：设备状态已设置为 WiFi配网中", GetTimeString().c_str());

    // --- 2. 设置BLE回调 ---
    if (!SetupBleCallbacks())
    {
        ESP_LOGE(TAG, "@EnterWifiConfigMode：设置 BLE 回调函数失败");
        application.Alert(Lang::Strings::ERROR, "BLE回调设置失败", "sad", Lang::Sounds::P3_ERR_PIN);
        return;
    }
    esp_task_wdt_reset(); // 重置看门狗

    // --- 3. 初始化并启动BLE广播 ---
    if (!InitializeAndStartBleAdvertising())
    {
        ESP_LOGE(TAG, "@EnterWifiConfigMode：初始化或启动 BLE 广播失败");
        application.Alert(Lang::Strings::ERROR, "BLE启动失败", "sad", Lang::Sounds::P3_ERR_PIN);
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(ble_config_mutex_);
        ble_config_state_ = BleConfigState::ADVERTISING;
        ESP_LOGI(TAG, "%s @EnterWifiConfigMode：状态更新为ADVERTISING", GetTimeString().c_str());
    }
    
    esp_task_wdt_reset(); // 重置看门狗

    // 记录BLE初始化后内存
    snapshot = get_memory_snapshot();
    log_memory_state(TAG, "@EnterWifiConfigMode：BLE 初始化后", snapshot);

    esp_task_wdt_reset(); // 重置看门狗

    // --- 4. UI提示和初始超时任务 ---
    UpdateUiForBleConfig(); // 更新UI显示，例如 "蓝牙配网模式" 和设备BLE名称
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：UI提示已更新", GetTimeString().c_str());
    esp_task_wdt_reset(); // 重置看门狗

    // 启动第一个配网超时语音提示任务
    if (!StartWifiConfigTimeoutTask())
    {
        ESP_LOGE(TAG, "@EnterWifiConfigMode：启动初始配网超时任务失败！");
        application.Alert(Lang::Strings::ERROR, "超时监控启动失败", "sad", "");
        return;
    }
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：初始配网超时任务已启动，等待 WiFi 凭据...", GetTimeString().c_str());
    esp_task_wdt_reset(); // 重置看门狗

    // --- 5. 主等待循环 ---
    const int check_interval_ms = 100;  // 缩短检查间隔，提高响应性
    int elapsed_ms_for_next_prompt = 0;
    const int timeout_ms_for_next_prompt_period = config_timeout_minutes_ * 60 * 1000;
    bool should_connect_wifi = false;

    while (true)
    {
        esp_task_wdt_reset(); // 在循环的开始喂狗
        
        // 使用互斥锁保护状态检查和更新
        {
            std::lock_guard<std::mutex> lock(ble_config_mutex_);
            
            // 如果标志已设置，复制到局部变量并重置
            if (should_connect_wifi_) {
                should_connect_wifi = true;
                should_connect_wifi_ = false;
                ESP_LOGI(TAG, "%s @EnterWifiConfigMode：检测到连接标志已设置，准备连接WiFi", 
                         GetTimeString().c_str());
            }
        }
        
        // 在互斥锁外执行耗时操作
        if (should_connect_wifi) {
            ESP_LOGI(TAG, "%s @EnterWifiConfigMode：开始连接WiFi - SSID: %s", 
                     GetTimeString().c_str(), ble_ssid_.c_str());
            
            // 执行WiFi连接
            bool connect_result = ConnectWifiByBle(ble_ssid_, ble_password_);
            
            // 更新状态
            std::lock_guard<std::mutex> lock(ble_config_mutex_);
            if (connect_result) {
                ble_config_state_ = BleConfigState::SUCCESS;
                ESP_LOGI(TAG, "%s @EnterWifiConfigMode：WiFi连接成功，状态更新为SUCCESS", 
                         GetTimeString().c_str());
            } else {
                ble_config_state_ = BleConfigState::FAILED;
                ESP_LOGI(TAG, "%s @EnterWifiConfigMode：WiFi连接失败，状态更新为FAILED", 
                         GetTimeString().c_str());
            }
        }

        // 检查是否到达下一个语音提示的超时时间
        if (elapsed_ms_for_next_prompt >= timeout_ms_for_next_prompt_period)
        {
            ESP_LOGW(TAG, "%s @EnterWifiConfigMode：内部等待凭据超时 (%d 分钟)，将触发下一次语音提示。", 
                     GetTimeString().c_str(), config_timeout_minutes_);
            ResetTimeoutTaskHandle();
            if (!StartWifiConfigTimeoutTask()) {
                ESP_LOGE(TAG, "@EnterWifiConfigMode：在内部超时后，无法重新启动超时任务！");
            }
            elapsed_ms_for_next_prompt = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_ms_for_next_prompt += check_interval_ms;
    }
} // End of EnterWifiConfigMode()

// 配网超时任务
static void WifiConfigTimeoutTask(void *pvParameters)
{
    // 检查参数，确保是 WifiBoard 类型
    WifiBoard *wifi_board = static_cast<WifiBoard *>(pvParameters);
    if (!wifi_board)
    {
        ESP_LOGE(TAG, "@WifiConfigTimeoutTask (%s)：【错误】无效的参数 (pvParameters is null)", GetTimeString().c_str());
        vTaskDelete(nullptr); // 删除自身
        return;
    }

    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【启动】配网超时任务启动，句柄: %p, 等待 %d 分钟。", 
             GetTimeString().c_str(), xTaskGetCurrentTaskHandle(), wifi_board->GetConfigTimeoutMinutes());
    
    // 使用 config_timeout_minutes_ 计算延时，确保与日志一致
    TickType_t delay_ticks = pdMS_TO_TICKS(static_cast<uint64_t>(wifi_board->GetConfigTimeoutMinutes()) * 60 * 1000);
    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：计算得到的延时 Ticks: %lu", GetTimeString().c_str(), delay_ticks);

    vTaskDelay(delay_ticks); // 等待超时

    // 超时发生
    ESP_LOGW(TAG, "@WifiConfigTimeoutTask (%s)：【超时】配网超时 (%d 分钟)!", 
             GetTimeString().c_str(), wifi_board->GetConfigTimeoutMinutes());

    // 标记任务句柄已失效，防止误删
    wifi_board->ResetTimeoutTaskHandle(); // 重置句柄，wifi_timeout_task_handle_ 置为 nullptr
    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【重置句柄】调用 ResetTimeoutTaskHandle 完成。", GetTimeString().c_str());

    // 调度Alert任务到Application主循环
    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【调度提示】准备调用 Application::GetInstance().Schedule() 来播放提示音。", GetTimeString().c_str());
    Application::GetInstance().Schedule([wifi_board]() {
        ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【执行调度内容】Application::Schedule 内的 lambda 开始执行。", GetTimeString().c_str());
        Application::GetInstance().Alert(
            Lang::Strings::BLE_CONFIG_MODE,
            "配网超时，请重试",
            "sad",
            Lang::Sounds::P3_WIFICONFIG); 
        ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【执行调度内容】Application::Alert 已调用。", GetTimeString().c_str());
    });
    
    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【调度完成】Application::GetInstance().Schedule() 已调用。", GetTimeString().c_str());

    // 短暂延时，确保 Application::Schedule 中的任务有机会被执行，提示音开始播放
    vTaskDelay(pdMS_TO_TICKS(100)); // 这个延时是为了给 Application 主循环处理 Schedule 的机会

    ESP_LOGI(TAG, "@WifiConfigTimeoutTask (%s)：【任务结束】超时语音提示流程已触发，任务即将自行删除。", GetTimeString().c_str());
    vTaskDelete(nullptr); // 任务结束
}


// <<<<<<<<<<<<<<<<<<<<<< BLE配网流程中的WiFi连接实现 【Begain】>>>>>>>>>>>>>>>>>>>>>>
/*
分为6个功能明确的小函数：
SaveWifiCredentials: 保存WiFi凭据到NVS和认证管理器
ReleaseBleResources: 释放BLE资源并等待完成
TakeEmergencyMeasures: 紧急措施处理
StartWifiAndWaitForConnection: 启动WiFi并等待连接
HandleWifiConnectSuccess: 处理WiFi连接成功
HandleWifiConnectFailure: 处理WiFi连接失败
*/
// 保存WiFi凭据到NVS和认证管理器
void WifiBoard::SaveWifiCredentials(const std::string &ssid, const std::string &password) {
    ESP_LOGI(TAG, "%s @SaveWifiCredentials：保存WiFi凭据 - SSID: %s", GetTimeString().c_str(), ssid.c_str());
    
    // 保存到NVS
    auto &ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
    ESP_LOGI(TAG, "%s @SaveWifiCredentials：WiFi凭据已保存到NVS", GetTimeString().c_str());
    
    // 短暂延时，让任务有时间处理完成当前操作
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_task_wdt_reset();
    
    // 添加认证信息
    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.AddAuth(std::string(ssid), std::string(password));
    ESP_LOGI(TAG, "%s @SaveWifiCredentials：WiFi认证信息已添加", GetTimeString().c_str());
}

// 释放BLE资源并等待完成
bool WifiBoard::ReleaseBleResources(const MemorySnapshot &initial_snapshot) {
    ESP_LOGI(TAG, "%s @ReleaseBleResources：开始释放BLE资源", GetTimeString().c_str());
    
    // 去初始化BLE模块并重置看门狗，确保BLE模块已完全停止运行。
    esp_task_wdt_reset();
    BleConfig::GetInstance().Deinitialize();    
    esp_task_wdt_reset();

    vTaskDelay(pdMS_TO_TICKS(100)); // 等待BLE资源彻底释放
    
    // 去初始化后，检查内存状态
    MemorySnapshot deinitialize_after_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "ReleaseBleResources：去初始化BLE后", deinitialize_after_snapshot);
    
    // 计算并输出内存差异
    MemorySnapshot diff = initial_snapshot.GetDifference(deinitialize_after_snapshot);
    ESP_LOGI(TAG, "去初始化BLE后: 内部RAM: %.2fKB, 总堆: %.2fKB", 
             diff.internal_ram / 1024.0f, diff.total_heap / 1024.0f);

    // 循环检查BLE是否完全释放
    bool ble_fully_released = false;    // 标记BLE是否完全释放
    int ble_release_retry = 0;          // 重试计数器
    const int ble_release_max_retry = 5;    // 最大重试次数
    int ble_force_deinit_count = 0;         // 强制去初始化计数器
    int last_free_sram = 0;                  // 上次内存值
    
    // 循环检查BLE是否完全释放
    for (; ble_release_retry < ble_release_max_retry; ++ble_release_retry) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
        
        // 获取当前内部RAM可用大小
        int free_sram_check = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        
        bool adv = BleConfig::GetInstance().IsAdvertising();
        bool running = BleConfig::ble_host_task_running;
        
        MemorySnapshot snapshot = get_memory_snapshot();
        log_memory_state(TAG, "@ReleaseBleResources：等待BLE彻底释放内存", snapshot);
        
        ESP_LOGI(TAG, "%s @ReleaseBleResources：BLE状态：Advertising=%d, Running=%d, 内存: %d字节, 重试: %d", 
                GetTimeString().c_str(), adv, running, free_sram_check, ble_release_retry);
        
        // 检查内存是否异常下降
        if (last_free_sram != 0 && free_sram_check < last_free_sram - 4096) {
            ESP_LOGW(TAG, "%s @ReleaseBleResources：内存异常下降，上次: %d, 本次: %d", 
                    GetTimeString().c_str(), last_free_sram, free_sram_check);
        }
        
        // 更新上次内存值
        last_free_sram = free_sram_check;
        
        if (!adv && !running && free_sram_check > 60000) {
            // BLE已停止且内存充足
            ESP_LOGI(TAG, "%s @ReleaseBleResources：BLE已停止且内存充足", GetTimeString().c_str());
            ble_fully_released = true;
            break;
        } 
        else if (!adv && !running) {
            // BLE已停止但内存不足
            ESP_LOGW(TAG, "%s @ReleaseBleResources：BLE已停止但内存不足(%d字节)", 
                    GetTimeString().c_str(), free_sram_check);
            heap_caps_check_integrity_all(true);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else {
            // BLE未完全停止
            ESP_LOGW(TAG, "%s @ReleaseBleResources：BLE未完全停止，重试中...(%d/%d)", 
                    GetTimeString().c_str(), ble_release_retry + 1, ble_release_max_retry);
            
            // 偶数次重试时强制清理
            if ((ble_release_retry + 1) % 2 == 0) {
                ESP_LOGW(TAG, "%s @ReleaseBleResources：强制清理BLE资源(第%d次)", 
                        GetTimeString().c_str(), ++ble_force_deinit_count);
                
                // 检查并强制结束BLE任务
                TaskHandle_t ble_task = BleConfig::GetBleHostTaskHandle();
                if (ble_task != NULL) {
                    ESP_LOGW(TAG, "%s @ReleaseBleResources：强制删除BLE任务", GetTimeString().c_str());
                    vTaskDelete(ble_task);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                heap_caps_check_integrity_all(true);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
    
    // 如果BLE未完全释放，采取紧急措施
    if (!ble_fully_released) {
        TakeEmergencyMeasures();
    }
    else {
        ESP_LOGI(TAG, "%s @ReleaseBleResources：BLE资源已彻底释放", GetTimeString().c_str());
    }
    
    return true;
}

// 紧急措施：强制清理BLE资源
void WifiBoard::TakeEmergencyMeasures() {
    ESP_LOGE(TAG, "%s @TakeEmergencyMeasures：BLE资源释放超时，采取紧急措施", GetTimeString().c_str());
    
    // 记录诊断信息
    MemorySnapshot current = get_memory_snapshot();
    log_memory_state(TAG, "BLE释放超时时内存状态", current);
    
    // 检查BLE任务状态
    TaskHandle_t ble_task = BleConfig::GetBleHostTaskHandle();
    if (ble_task != NULL) {
        // 获取任务信息
        TaskStatus_t task_status;
        vTaskGetInfo(ble_task, &task_status, pdTRUE, eInvalid);
        ESP_LOGE(TAG, "BLE任务状态: 优先级=%u, 状态=%d, 高水位=%u", 
            (unsigned int)task_status.uxCurrentPriority, 
            (int)task_status.eCurrentState, 
            (unsigned int)task_status.usStackHighWaterMark);
        
        // 强制删除任务
        ESP_LOGE(TAG, "%s @TakeEmergencyMeasures：强制终止BLE任务", GetTimeString().c_str());
        vTaskDelete(ble_task);
    }
    
    // 强制执行内存整理
    ESP_LOGI(TAG, "%s @TakeEmergencyMeasures：执行强制内存整理", GetTimeString().c_str());
    heap_caps_check_integrity_all(true);
    
    // 短暂延时让系统稳定
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // 记录措施后的内存状态
    MemorySnapshot after = get_memory_snapshot();
    log_memory_state(TAG, "紧急措施后内存状态", after);
    ESP_LOGI(TAG, "%s @TakeEmergencyMeasures：内存增加: %d字节", 
            GetTimeString().c_str(), (int)(after.internal_ram - current.internal_ram));
}

// 启动WiFi并等待连接
bool WifiBoard::StartWifiAndWaitForConnection() {
    ESP_LOGI(TAG, "%s @StartWifiAndWaitForConnection：准备启动WiFi", GetTimeString().c_str());
    
    // 启动WiFi连接
    MemorySnapshot before_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "StartWifiAndWaitForConnection：启动WiFi前", before_snapshot);
    esp_task_wdt_reset();
    
    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.Start();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    esp_task_wdt_reset();
    MemorySnapshot after_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "StartWifiAndWaitForConnection：启动WiFi后", after_snapshot);
    
    // 等待连接结果
    ESP_LOGI(TAG, "%s @StartWifiAndWaitForConnection：等待WiFi连接结果(8秒)", GetTimeString().c_str());
    bool connected = wifi_station.WaitForConnected(8000);
    esp_task_wdt_reset();
    
    return connected;
}

// 处理WiFi连接成功
void WifiBoard::HandleWifiConnectSuccess() {
    auto &wifi_station = WifiStation::GetInstance();
    ESP_LOGI(TAG, "%s @HandleWifiConnectSuccess：WiFi连接成功，IP: %s", 
             GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
    
    // 发送连接成功状态
    esp_task_wdt_reset();
    BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTED);
    
    // 短暂延时确保状态发送成功
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_task_wdt_reset();
    
    // 删除超时任务
    if (wifi_timeout_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "%s @HandleWifiConnectSuccess：删除配网超时任务", GetTimeString().c_str());
        vTaskDelete(wifi_timeout_task_handle_);
        wifi_timeout_task_handle_ = nullptr;
    }
    
    // 设置设备状态为空闲
    ESP_LOGI(TAG, "%s @HandleWifiConnectSuccess：设置设备状态为空闲", GetTimeString().c_str());
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "%s @HandleWifiConnectSuccess：配网成功完成", GetTimeString().c_str());
}

// 处理WiFi连接失败
void WifiBoard::HandleWifiConnectFailure() {
    ESP_LOGW(TAG, "%s @HandleWifiConnectFailure：WiFi连接失败", GetTimeString().c_str());
    
    // 停止WiFi连接
    esp_task_wdt_reset();
    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.Stop();
    
    // 设置失败状态码
    wifi_config_status_t status = WIFI_STATUS_FAIL_CONN;
    
    // 重新初始化BLE并恢复广播
    ESP_LOGI(TAG, "%s @HandleWifiConnectFailure：重新初始化BLE并恢复广播", GetTimeString().c_str());
    BleConfig::GetInstance().Initialize();
    vTaskDelay(pdMS_TO_TICKS(200));
    BleConfig::GetInstance().StartAdvertising();
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // 发送WiFi连接失败状态
    ESP_LOGW(TAG, "%s @HandleWifiConnectFailure：发送失败状态: %d", GetTimeString().c_str(), status);
    BleConfig::GetInstance().SendWifiStatus(status);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 显示错误信息
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        std::string error_msg = "WiFi连接失败";
        display->ShowNotification(error_msg.c_str(), 3000);
    }
    
    // 设置设备状态为配网中
    ESP_LOGI(TAG, "%s @HandleWifiConnectFailure：恢复设备状态为配网中", GetTimeString().c_str());
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
}

// 主函数：BLE配网流程中的WiFi连接实现
bool WifiBoard::ConnectWifiByBle(const std::string &ssid, const std::string &password)
{
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：BLE配网流程 - 准备连接WiFi SSID: %s", 
             GetTimeString().c_str(), ssid.c_str());
    
    // 检查当前内存状态
    MemorySnapshot connectWifi_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "ConnectWifiByBle：连接WiFi前，初始内存状态", connectWifi_snapshot);
    esp_task_wdt_reset();

    // 1. 保存WiFi凭据
    ESP_LOGI(TAG, "%s @ConnectWifiByBle # SaveWifiCredentials：保存WiFi凭据到NVS", GetTimeString().c_str());
    SaveWifiCredentials(ssid, password);
    
    // 2. 释放BLE资源
    ESP_LOGI(TAG, "%s @ConnectWifiByBle # ReleaseBleResources：释放BLE资源", GetTimeString().c_str());
    ReleaseBleResources(connectWifi_snapshot);
    
    // 3. 启动WiFi并等待连接
    ESP_LOGI(TAG, "%s @ConnectWifiByBle # StartWifiAndWaitForConnection：启动WiFi并等待连接", GetTimeString().c_str());
    bool connected = StartWifiAndWaitForConnection();
    
    if (connected) {
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi连接成功", GetTimeString().c_str());
        
        // 更新设备状态
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        
        // 清理超时任务
        if (wifi_timeout_task_handle_ != nullptr) {
            vTaskDelete(wifi_timeout_task_handle_);
            wifi_timeout_task_handle_ = nullptr;
        }
        
        return true;
    } else {
        ESP_LOGW(TAG, "%s @ConnectWifiByBle：WiFi连接失败", GetTimeString().c_str());
        
        // 发送失败状态到BLE客户端
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_FAIL);
        
        return false;
    }
}


// <<<<<<<<<<<<<<<<<<<<<< BLE配网流程中的WiFi连接实现 【END】>>>>>>>>>>>>>>>>>>>>>>



// 尝试连接已保存的 WiFi 网络
bool WifiBoard::TryConnectSavedWifi()
{
    // 注意：此方法由 StartNetwork 调用，看门狗已由 StartNetwork 中的 WdtGuard 管理
    ESP_LOGI(TAG, "%s @TryConnectSavedWifi：尝试连接已保存的 WiFi 网络", GetTimeString().c_str());

    auto &wifi_station = WifiStation::GetInstance(); // 获取 WiFi 实例

    // 设置 WiFi 扫描开始（回调函数）
    wifi_station.OnScanBegin([this]() { // 使用 lambda 表达式捕获 this 指针，以访问成员变量
        ESP_LOGI(TAG, "%s @TryConnectSavedWifi.OnScanBegin：WiFi 扫描开始", GetTimeString().c_str());

        auto display = Board::GetInstance().GetDisplay(); // 获取显示实例
        if (display)
            display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000); // 显示扫描提示，持续 30 秒
    });

    // 设置 WiFi 连接开始（回调函数）
    wifi_station.OnConnect([this](const std::string &ssid) { // 使用 lambda 表达式捕获 this 指针，以访问成员变量
        ESP_LOGI(TAG, "%s @TryConnectSavedWifi.OnConnect：开始连接 WiFi: %s", GetTimeString().c_str(), ssid.c_str());

        auto display = Board::GetInstance().GetDisplay(); // 获取显示实例
        if (display)
        {
            std::string notification = Lang::Strings::CONNECT_TO;   // 连接提示
            notification += ssid;                                   // 添加 SSID
            notification += "...";                                  // 添加连接中
            display->ShowNotification(notification.c_str(), 30000); // 显示连接提示，持续 30 秒
        }
    });

    // 设置 WiFi 连接成功（回调函数）
    wifi_station.OnConnected([this](const std::string &ssid) { // 使用 lambda 表达式捕获 this 指针，以访问成员变量
        ESP_LOGI(TAG, "%s @TryConnectSavedWifi.OnConnected：WiFi 连接成功: %s", GetTimeString().c_str(), ssid.c_str());

        auto display = Board::GetInstance().GetDisplay(); // 获取显示实例
        if (display)
        { // 添加判空
            std::string notification = Lang::Strings::CONNECTED_TO;
            notification += ssid;
            display->ShowNotification(notification.c_str(), 30000);
        }
    });

    wifi_station.Start(); // 用 NVS 保存的配置信息，启动尝试连接 WiFi
    ESP_LOGI(TAG, "%s @TryConnectSavedWifi：[启动连接WiFi]用 NVS 保存的配置信息，启动尝试连接 WiFi", GetTimeString().c_str());

    // 等待连接结果
    ESP_LOGI(TAG, "%s @TryConnectSavedWifi：[等待 WiFi 连接]等待时间: 6 秒", GetTimeString().c_str());
    bool connected = wifi_station.WaitForConnected(6 * 1000);

    // 检查连接结果
    if (connected)
    {
        ESP_LOGI(TAG, "%s @TryConnectSavedWifi：[WiFi 连接成功]用保存在 NVS 的配置，连接 WiFi 成功，IP: %s", GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
        return true; // 连接成功
    }
    else
    {
        ESP_LOGW(TAG, "%s @TryConnectSavedWifi：[WiFi 连接失败] 用保存在 NVS 的配置，连接 WiFi 失败", GetTimeString().c_str());
        wifi_station.Stop(); // 停止尝试连接
        return false;        // 连接失败
    }
}

// 启动配网模式
void WifiBoard::StartConfigMode()
{
    // 注意：此方法由 StartNetwork 调用，看门狗已由 StartNetwork 中的 WdtGuard 管理
    ESP_LOGI(TAG, "%s @StartConfigMode：启动配网模式", GetTimeString().c_str());

    // 重置看门狗，防止播放提示音过程中触发超时
    esp_task_wdt_reset();

    // 播放BLE配网提示音
    auto &application = Application::GetInstance();
    ESP_LOGI(TAG, "%s @StartConfigMode：播放BLE配网提示音", GetTimeString().c_str());
    application.PlaySound(Lang::Sounds::P3_WELCOME);
    // application.PlaySound(Lang::Sounds::P3_WIFI_CONFIG_REQUIRED);

    // 重置看门狗，防止延时过程中触发超时
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(500)); // 稍作延时
    esp_task_wdt_reset();           // 延时后再次重置看门狗

    // 进入 BLE 配网模式
    wifi_config_mode_ = true; // 标记进入配网模式
    ESP_LOGI(TAG, "%s @StartConfigMode：准备进入BLE配网模式", GetTimeString().c_str());
    esp_task_wdt_reset();  // 进入配网模式前重置看门狗
    EnterWifiConfigMode(); // 进入 BLE 配网模式
}

// 主网络启动方法
void WifiBoard::StartNetwork()
{
    // 创建 WdtGuard 对象，自动注册看门狗，函数结束时自动注销
    WdtGuard wdt_guard;

    ESP_LOGI(TAG, "%s @StartNetwork：开始启动网络", GetTimeString().c_str());

    // 检查 NVS 中是否有 WiFi 配置
    ESP_LOGI(TAG, "%s @StartNetwork：检查 NVS 中的 WiFi 凭据（SSID 列表）...", GetTimeString().c_str());
    auto &ssid_manager = SsidManager::GetInstance(); // 获取 SSID 管理器实例
    auto ssid_list = ssid_manager.GetSsidList();     // 获取 SSID 列表
    bool nvs_is_empty = ssid_list.empty();           // 检查 NVS 是否为空

    ESP_LOGI(TAG, "%s @StartNetwork：NVS 中 SSID 数量: %d", GetTimeString().c_str(), ssid_list.size());

    // 检查是否强制进入配网模式 (例如长按按钮触发)
    if (wifi_config_mode_)
    {
        ESP_LOGI(TAG, "%s @StartNetwork：检测到强制配网标志，直接进入配网模式", GetTimeString().c_str());
        StartConfigMode();
        return; // 函数结束，WdtGuard 析构函数自动注销看门狗
    }

    // 如果 NVS 为空，直接进入配网模式
    if (nvs_is_empty)
    {
        ESP_LOGI(TAG, "%s @StartNetwork：NVS 为空，进入配网模式", GetTimeString().c_str());
        StartConfigMode();
        return; // 函数结束，WdtGuard 析构函数自动注销看门狗
    }

    // 如果 NVS 非空，尝试连接 WiFi
    ESP_LOGI(TAG, "%s @StartNetwork：NVS 非空，尝试连接已保存的 WiFi", GetTimeString().c_str());
    bool connected = TryConnectSavedWifi();

    // 如果连接失败，进入配网模式
    if (!connected)
    {
        ESP_LOGI(TAG, "%s @StartNetwork：连接已保存的 WiFi 失败，进入配网模式", GetTimeString().c_str());
        StartConfigMode();
    }
    // 连接成功则直接返回，继续正常启动流程
    // 函数结束时，WdtGuard 析构函数自动注销看门狗
}

Http *WifiBoard::CreateHttp()
{
    ESP_LOGD(TAG, "@CreateHttp：创建 HTTP 客户端");
    return new EspHttp();
}

WebSocket *WifiBoard::CreateWebSocket()
{
    ESP_LOGD(TAG, "@CreateWebSocket：创建 WebSocket 客户端");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    std::string url = CONFIG_WEBSOCKET_URL;
    ESP_LOGI(TAG, "%s @CreateWebSocket：WebSocket URL: %s", GetTimeString().c_str(), url.c_str());
    if (url.find("wss://") == 0)
    {
        ESP_LOGI(TAG, "%s @CreateWebSocket：使用 TLS 传输层创建安全 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TlsTransport());
    }
    else
    {
        ESP_LOGI(TAG, "%s @CreateWebSocket：使用 TCP 传输层创建普通 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TcpTransport());
    }
#endif
    ESP_LOGW(TAG, "@CreateWebSocket：WebSocket 未配置，返回 nullptr");
    return nullptr;
}

Mqtt *WifiBoard::CreateMqtt()
{
    ESP_LOGD(TAG, "@CreateMqtt：创建 MQTT 客户端");
    return new EspMqtt();
}

Udp *WifiBoard::CreateUdp()
{
    ESP_LOGD(TAG, "@CreateUdp：创建 UDP 客户端");
    return new EspUdp();
}

const char *WifiBoard::GetNetworkStateIcon()
{
    if (wifi_config_mode_)
    {
        ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 配网模式");
        return FONT_AWESOME_WIFI;
    }
    auto &wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected())
    {
        ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 未连接");
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 已连接，信号强度: %d dBm", rssi);
    if (rssi >= -60)
    {
        return FONT_AWESOME_WIFI;
    }
    else if (rssi >= -70)
    {
        return FONT_AWESOME_WIFI_FAIR;
    }
    else
    {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson()
{
    ESP_LOGD(TAG, "@GetBoardJson：获取板卡 JSON 信息");
    // Set the board type for OTA
    auto &wifi_station = WifiStation::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_)
    {
        board_json += "\"ssid\":\"" + wifi_station.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_station.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_station.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_station.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    ESP_LOGD(TAG, "@GetBoardJson：板卡 JSON: %s", board_json.c_str());
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled)
{
    ESP_LOGI(TAG, "%s @SetPowerSaveMode：设置 WiFi 省电模式: %s", GetTimeString().c_str(), enabled ? "启用" : "禁用");
    auto &wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration()
{
    ESP_LOGI(TAG, "%s @ResetWifiConfiguration：重置 WiFi 配置", GetTimeString().c_str());
    // Set a flag and reboot the device to enter the network configuration mode
    {
        ESP_LOGI(TAG, "%s @ResetWifiConfiguration：设置强制配网标志 force_ap=1", GetTimeString().c_str());
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    ESP_LOGI(TAG, "%s @ResetWifiConfiguration：显示进入配网模式提示", GetTimeString().c_str());
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    ESP_LOGI(TAG, "%s @ResetWifiConfiguration：等待 1 秒后重启设备", GetTimeString().c_str());
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    ESP_LOGI(TAG, "%s @ResetWifiConfiguration：重启设备以进入配网模式", GetTimeString().c_str());
    esp_restart();
}
