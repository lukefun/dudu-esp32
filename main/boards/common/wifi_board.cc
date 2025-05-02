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

#include "application.h"    // <--- 包含 Application 头文件
#include "assets/lang_config.h" // <--- 包含语言配置头文件
#include "ble_config/ble_config.h"
#include "board.h"              // <--- 包含 Board 头文件
#include "display.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_configuration_ap.h"
#include "wifi_station.h"

static const char *TAG = "WifiBoard";    // <--- 确保 TAG 已定义

// Forward declaration for timeout task
static void WifiConfigTimeoutTask(void *pvParameters);

static std::string GetTimeString() {
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    // 转换为本地时间
    tm* tm_info = localtime(&now_time_t);

    // 格式化时间字符串（含毫秒）
    char buffer[64];
    strftime(buffer, sizeof(buffer), "[%H:%M:%S", tm_info);
    int len = strlen(buffer);
    snprintf(buffer + len, sizeof(buffer) - len, ".%03d]", static_cast<int>(ms.count()));

    return std::string(buffer);
}

WifiBoard::WifiBoard() {
    ESP_LOGI(TAG, "%s @WifiBoard：初始化 WifiBoard", GetTimeString().c_str());
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "%s @WifiBoard：检测到强制配网标志 force_ap=1，重置为0", GetTimeString().c_str());
        settings.SetInt("force_ap", 0);
    }
    ESP_LOGI(TAG, "%s @WifiBoard：WifiBoard 初始化完成，配网模式状态: %s", GetTimeString().c_str(), wifi_config_mode_ ? "启用" : "禁用");
}

std::string WifiBoard::GetBoardType() {
    ESP_LOGD(TAG, "@GetBoardType：获取板卡类型: wifi");
    return "wifi";
}

// --- 重构 EnterWifiConfigMode 的辅助函数实现 ---
// 设置 BLE 回调函数
bool WifiBoard::SetupBleCallbacks() {
    ESP_LOGI(TAG, "%s @SetupBleCallbacks：设置 BLE 回调函数", GetTimeString().c_str());
    auto& ble_config = BleConfig::GetInstance();

    // 1. 设置 WiFi 凭据接收回调
    ble_config.SetCredentialsReceivedCallback([this](const std::string& ssid, const std::string& password) {
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.CredentialsReceivedCallback：收到 WiFi 凭据 - SSID: %s", GetTimeString().c_str(), ssid.c_str());
        ble_ssid_ = ssid;
        ble_password_ = password;
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.CredentialsReceivedCallback：WiFi 凭据已暂存", GetTimeString().c_str());
    });

    // 2. 设置开始连接 WiFi 的回调
    ble_config.SetConnectWifiCallback([this]() {
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：收到连接 WiFi 命令", GetTimeString().c_str());
        int free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：连接WiFi前内存: %d 字节", GetTimeString().c_str(), free_heap);

        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTING);

        if (ble_ssid_.empty() || ble_password_.empty()) {
            ESP_LOGW(TAG, "@SetupBleCallbacks.ConnectWifiCallback：BLE配网凭据为空，无法连接");
            BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_FAIL_SSID);
            return;
        }

        BleConfig::GetInstance().StopAdvertising();
        ESP_LOGI(TAG, "%s @SetupBleCallbacks.ConnectWifiCallback：已停止BLE广播，准备连接WiFi", GetTimeString().c_str());
        esp_task_wdt_reset();
        bool wifi_ok = false;
        try {
            ConnectWifiByBle(ble_ssid_, ble_password_);
            wifi_ok = true;
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "@SetupBleCallbacks.ConnectWifiCallback：ConnectWifiByBle异常: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "@SetupBleCallbacks.ConnectWifiCallback：ConnectWifiByBle发生未知异常");
        }
        if (!wifi_ok) {
            ESP_LOGW(TAG, "@SetupBleCallbacks.ConnectWifiCallback：ConnectWifiByBle返回失败，WiFi未连接");
            BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_FAIL_SSID);
        }
    });

    ESP_LOGI(TAG, "%s @SetupBleCallbacks：BLE 回调设置完成", GetTimeString().c_str());

    return true;
}

// 连接WiFi，启动配网超时任务，并在成功或失败时更新UI
bool WifiBoard::InitializeAndStartBleAdvertising() {
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：初始化并启动 BLE 广播", GetTimeString().c_str());
    auto& ble_config = BleConfig::GetInstance();          // 获取 ble_config 实例
    // auto& application = Application::GetInstance();    // 获取 application 实例 (已注释掉，因为后续代码未使用)

    // 检查内存并采取保守策略：如果内存不足，先进行堆检查
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); // 获取当前可用内存
    if (free_sram < 60000) {
        ESP_LOGW(TAG, "%s @InitializeAndStartBleAdvertising：内存较低 (%d字节)，采用保守策略", GetTimeString().c_str(), free_sram);

        heap_caps_check_integrity_all(true);          // 进行堆检查
        // 当设置为 true 时，如果该函数检测到任何堆损坏 (Heap Corruption)，它会 立即调用 abort() 函数，
        // 直接让程序中止执行 。它不会返回错误代码让您的程序去判断和处理，而是直接停止。
        // 这是为了确保在内存不足的情况下，程序不会继续执行可能导致不可预测的后果的代码。
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 初始化 BLE
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：开始初始化 BLE", GetTimeString().c_str());
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);   // 获取当前可用内存
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：BLE 初始化前内存: %d 字节", GetTimeString().c_str(), free_sram);
    ble_config.Initialize();                                    // 初始化 BLE
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：BLE 初始化完成", GetTimeString().c_str());
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);   // 获取当前可用内存
    ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：BLE 初始化后内存: %d 字节", GetTimeString().c_str(), free_sram);
    vTaskDelay(pdMS_TO_TICKS(300));                             // 短暂延迟，确保 BLE 初始化完成

    // 开始广播 (注释掉：Initialize() 内部已通过回调启动)
    // ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：开始 BLE 广播", GetTimeString().c_str());
    // if (!ble_config.StartAdvertising()) {                  // 启动 BLE 广播
    //     ESP_LOGE(TAG, "@InitializeAndStartBleAdvertising：BLE 广播启动失败");
    //     application.Alert("蓝牙配网启动失败", "请重启设备", "", Lang::Sounds::P3_ERR_PIN);
    //     return false; // 返回失败
    // }
    // ESP_LOGI(TAG, "%s @InitializeAndStartBleAdvertising：BLE 广播已启动", GetTimeString().c_str());
    return true; // 返回成功
}

// 更新 UI 以显示 BLE 配网状态
void WifiBoard::UpdateUiForBleConfig() {
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：更新 UI 及播放提示音", GetTimeString().c_str());
    auto& application = Application::GetInstance();

    // 优化提示合成，合并为一条 Alert，BLE 设备名可灵活配置
    std::string ble_hint;
    if (Lang::Strings::CONNECT_TO_BLE && Lang::Strings::CONNECT_TO_BLE[0] != '\0') {
        ble_hint = Lang::Strings::CONNECT_TO_BLE;
    } else {
        ESP_LOGW(TAG, "@UpdateUiForBleConfig：未找到 CONNECT_TO_BLE 字符串");
        ble_hint = "请使用支持BLE的手机App扫描并连接设备：";
    }
    // 合成最终提示
    ble_hint += std::string(" ") + kBleDeviceName;
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：配网提示: %s", GetTimeString().c_str(), ble_hint.c_str());

    // 合并为一次 Alert，减少 UI 闪烁和音频重叠
    application.Alert(Lang::Strings::BLE_CONFIG_MODE, ble_hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);
    ESP_LOGI(TAG, "%s @UpdateUiForBleConfig：显示 BLE 配网提示 和 播放提示音：\"进入配网模式\"完成", GetTimeString().c_str());
}

// 启动配网超时任务
bool WifiBoard::StartWifiConfigTimeoutTask() {
    ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：启动配网超时任务", GetTimeString().c_str());
    // 检查是否已经存在任务句柄
    if (wifi_timeout_task_handle_ == nullptr) { // 如果句柄为空
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：创建配网超时任务 (%d 分钟)", GetTimeString().c_str(), config_timeout_minutes_);
        // 创建任务
        BaseType_t result = xTaskCreate(WifiConfigTimeoutTask, "WifiTimeoutTask", 4096, this, 5, &wifi_timeout_task_handle_);

        // 检查任务创建结果
        if (result != pdPASS) {
            ESP_LOGE(TAG, "@StartWifiConfigTimeoutTask：创建超时任务失败，错误码: %d", result);
            wifi_timeout_task_handle_ = nullptr; // 确保句柄为空
            return false; // 返回失败
        }
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：超时任务创建成功", GetTimeString().c_str());
        return true; // 返回成功
    } else {
        ESP_LOGW(TAG, "%s @StartWifiConfigTimeoutTask：发现旧的配网超时任务句柄，将尝试删除并创建新任务", GetTimeString().c_str());
        // 尝试删除旧任务
        vTaskDelete(wifi_timeout_task_handle_);
        // 将句柄置空，以便后续创建新任务
        wifi_timeout_task_handle_ = nullptr; 
        
        // --- 复用创建任务的代码 --- 
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：创建新的配网超时任务 (%d 分钟)", GetTimeString().c_str(), config_timeout_minutes_);
        // 创建任务
        BaseType_t result = xTaskCreate(WifiConfigTimeoutTask, "WifiTimeoutTask", 4096, this, 5, &wifi_timeout_task_handle_);
        // 检查任务创建结果
        if (result != pdPASS) {
            ESP_LOGE(TAG, "@StartWifiConfigTimeoutTask：创建新超时任务失败，错误码: %d", result);
            wifi_timeout_task_handle_ = nullptr; // 确保句柄为空
            return false; // 返回失败
        }
        ESP_LOGI(TAG, "%s @StartWifiConfigTimeoutTask：新超时任务创建成功", GetTimeString().c_str());
        return true; // 返回成功
    }
}

// 进入 WiFi 配网模式 (同步实现)
void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：进入 WiFi 配网模式 (同步版)", GetTimeString().c_str());

    // 1. 准备阶段：内存检查和状态设置
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：内存状态 - 可用: %u, 最小: %u", GetTimeString().c_str(), free_sram, min_free_sram);

    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);     // 设置设备状态为配网中
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：设备状态设置为 WiFi配网中", GetTimeString().c_str());

    // 2. 设置 BLE 回调
    if (!SetupBleCallbacks()) {
        ESP_LOGE(TAG, "@EnterWifiConfigMode：设置 BLE 回调失败");
        application.Alert(Lang::Strings::ERROR, "BLE回调设置失败", "sad", Lang::Sounds::P3_ERR_PIN);
        return; // 无法继续
    }

    // 3. 初始化并启动 BLE 广播
    if (!InitializeAndStartBleAdvertising()) {
        ESP_LOGE(TAG, "@EnterWifiConfigMode：初始化或启动 BLE 广播失败");
        return; // 无法继续
    }

    // 4. 播放提示音和显示信息
    UpdateUiForBleConfig();
    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：播放提示音和显示BLE配网状态完成！", GetTimeString().c_str());

    // 5. 等待 WiFi 凭据和连接命令
    const int timeout_ms = config_timeout_minutes_ * 60 * 1000; // 转换为毫秒
    const int check_interval_ms = 1000; // 每秒检查一次
    int elapsed_ms = 0;

    ESP_LOGI(TAG, "%s @EnterWifiConfigMode：等待 WiFi 凭据，超时时间: %d 分钟", GetTimeString().c_str(), config_timeout_minutes_);

    while (elapsed_ms < timeout_ms) {
        // 检查是否收到凭据
        if (!ble_ssid_.empty() && !ble_password_.empty()) {
            ESP_LOGI(TAG, "%s @EnterWifiConfigMode：收到 WiFi 凭据，准备连接", GetTimeString().c_str());
            
            // 停止 BLE 广播
            BleConfig::GetInstance().StopAdvertising();
            esp_task_wdt_reset(); // 重置看门狗

            // 尝试连接 WiFi
            ESP_LOGI(TAG, "%s @EnterWifiConfigMode：开始连接 WiFi: %s", GetTimeString().c_str(), ble_ssid_.c_str());
            BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTING);

            // 连接 WiFi
            ConnectWifiByBle(ble_ssid_, ble_password_);
            ESP_LOGI(TAG, "%s @EnterWifiConfigMode：WiFi 连接流程已执行", GetTimeString().c_str());
            return; // 连接流程已执行，退出函数
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_ms += check_interval_ms;
        esp_task_wdt_reset(); // 定期重置看门狗
    }

    // 超时处理
    ESP_LOGW(TAG, "%s @EnterWifiConfigMode：配网超时 (%d 分钟)", GetTimeString().c_str(), config_timeout_minutes_);
    
    // 停止 BLE 并清理资源
    BleConfig::GetInstance().StopAdvertising();
    vTaskDelay(pdMS_TO_TICKS(200));
    BleConfig::GetInstance().Deinitialize();

    // 显示超时提示
    application.Alert(Lang::Strings::ERROR, "配网超时", "sad", Lang::Sounds::P3_EXCLAMATION);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGW(TAG, "%s @EnterWifiConfigMode：配网超时，准备重启设备...", GetTimeString().c_str());
    esp_restart();
}

// 配网超时任务
static void WifiConfigTimeoutTask(void *pvParameters) {
    WifiBoard *wifi_board = static_cast<WifiBoard *>(pvParameters);
    if (!wifi_board) {
        ESP_LOGE(TAG, "@WifiConfigTimeoutTask：无效的参数");
        vTaskDelete(nullptr); // 删除自身
        return;
    }

    ESP_LOGI(TAG, "%s @WifiConfigTimeoutTask：配网超时任务启动，等待 %d 分钟", GetTimeString().c_str(), wifi_board->GetConfigTimeoutMinutes());
    vTaskDelay(pdMS_TO_TICKS(wifi_board->GetConfigTimeoutMinutes() * 60 * 1000));

    // 超时发生
    ESP_LOGW(TAG, "%s @WifiConfigTimeoutTask：配网超时 (%d 分钟)!", GetTimeString().c_str(), wifi_board->GetConfigTimeoutMinutes());

    // 标记任务句柄已失效，防止误删
    wifi_board->ResetTimeoutTaskHandle(); // 调用公共方法重置句柄 

    // 停止 BLE
    ESP_LOGI(TAG, "%s @WifiConfigTimeoutTask：停止 BLE 广播并去初始化", GetTimeString().c_str());
    BleConfig::GetInstance().StopAdvertising();
    // 短暂延时确保广播停止
    vTaskDelay(pdMS_TO_TICKS(200)); 
    BleConfig::GetInstance().Deinitialize();

    // 发送超时状态（如果需要，但重启通常足够）
    // BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_TIMEOUT); 

    // 显示超时提示
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().Alert(Lang::Strings::ERROR, "配网超时", "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待提示显示

    ESP_LOGW(TAG, "%s @WifiConfigTimeoutTask：配网超时，准备重启设备...", GetTimeString().c_str());
    esp_restart();

    // 理论上不会执行到这里，但在 esp_restart 失败时删除任务
    vTaskDelete(nullptr);
}

// 新增：BLE配网流程中的WiFi连接实现 - 优化内存使用，避免栈溢出
void WifiBoard::ConnectWifiByBle(const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：BLE配网流程 - 准备连接WiFi SSID: %s", GetTimeString().c_str(), ssid.c_str());
    
    // 检查当前内存状态
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：连接WiFi前内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", 
             GetTimeString().c_str(), free_sram, min_free_sram);
    
    // 先保存WiFi凭据到NVS
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi凭据已保存到NVS", GetTimeString().c_str());
    
    // 短暂延时，让BLE任务有时间处理完成当前操作
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 添加认证信息
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.AddAuth(std::string(ssid), std::string(password));
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi认证信息已添加", GetTimeString().c_str());
    
    // 再次检查内存状态
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：启动WiFi前内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", 
             GetTimeString().c_str(), free_sram, min_free_sram);
    
    // 如果可用内存过低，尝试释放一些内存
    if (free_sram < 60000) {
        ESP_LOGW(TAG, "%s @ConnectWifiByBle：可用内存较低，尝试释放资源...", GetTimeString().c_str());
        // 停止BLE广播以释放资源
        BleConfig::GetInstance().StopAdvertising();
        // 强制执行垃圾回收
        heap_caps_check_integrity_all(true);
        // 再次检查内存
        free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：释放资源后内存状态 - 当前可用: %u 字节", GetTimeString().c_str(), free_sram);
    }
    
    // 重置任务看门狗，防止WiFi初始化过程中触发看门狗超时
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：重置任务看门狗（1），防止WiFi初始化过程中触发看门狗超时", GetTimeString().c_str());
    
    // 启动WiFi
    wifi_station.Start();
    vTaskDelay(pdMS_TO_TICKS(500)); // 短暂延时，让WiFi任务有时间初始化
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi已启动", GetTimeString().c_str());

    // 重置任务看门狗
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：重置任务看门狗（2），防止WiFi连接过程中触发看门狗超时", GetTimeString().c_str());
    
    // 等待连接结果，使用更长的超时时间
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：等待WiFi连接结果，超时时间: 20秒", GetTimeString().c_str());
    bool connected = wifi_station.WaitForConnected(20000); // 增加到20秒

    // 连接过程完成，再次重置看门狗
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "%s @ConnectWifiByBle：重置任务看门狗（3），防止WiFi连接过程中触发看门狗超时", GetTimeString().c_str());

    if (connected) {
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi连接成功，IP: %s", GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
        // 发送连接成功状态
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTED);
        // 短暂延时确保状态发送成功
        vTaskDelay(pdMS_TO_TICKS(500));
        // 删除超时任务
        if (wifi_timeout_task_handle_ != nullptr) {
            ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi连接成功，删除配网超时任务", GetTimeString().c_str());
            vTaskDelete(wifi_timeout_task_handle_);
            wifi_timeout_task_handle_ = nullptr;
        }
        // 去初始化BLE模块
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：去初始化 BLE 模块", GetTimeString().c_str());
        BleConfig::GetInstance().Deinitialize();
        // 设置设备状态为空闲
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：设置设备状态为空闲", GetTimeString().c_str());
        Application::GetInstance().SetDeviceState(kDeviceStateIdle); // 使用正确的状态枚举
        // 不再重启设备，配网成功后应进入正常工作流程
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：配网成功完成，进入正常工作状态", GetTimeString().c_str());
    } else {
        ESP_LOGW(TAG, "%s @ConnectWifiByBle：WiFi连接失败，重新进入配网状态！", GetTimeString().c_str());
        // 停止WiFi连接
        wifi_station.Stop();
        
        // 发送连接失败状态
        wifi_config_status_t status = WIFI_STATUS_FAIL_CONN;
        if (!wifi_station.IsConnected()) {
            // 如果连接失败，默认使用一般连接失败状态
            status = WIFI_STATUS_FAIL_CONN;
        }
        
        // 确保BLE模块处于可用状态
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：检查 BLE 是否正在广播", GetTimeString().c_str());
        if (!BleConfig::GetInstance().IsAdvertising()) {
            ESP_LOGI(TAG, "%s @ConnectWifiByBle：重新启动BLE广播", GetTimeString().c_str());
            BleConfig::GetInstance().StartAdvertising();
            // 给BLE一些时间来启动
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        
        // 发送失败状态并等待足够长的时间确保发送成功
        ESP_LOGW(TAG, "%s @ConnectWifiByBle：发送WiFi连接失败状态: %d", GetTimeString().c_str(), status);
        BleConfig::GetInstance().SendWifiStatus(status);
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // 显示连接失败提示
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            std::string error_msg;
            switch (status) {
                case WIFI_STATUS_FAIL_AP_NOT_FOUND:
                    error_msg = "找不到WiFi热点";
                    break;
                case WIFI_STATUS_FAIL_AUTH:
                    error_msg = "WiFi密码错误";
                    break;
                default:
                    error_msg = "WiFi连接失败";
                    break;
            }
            display->ShowNotification(error_msg.c_str(), 3000);
        }
        // 恢复设备状态为配网中，允许用户在超时前重试
        ESP_LOGI(TAG, "%s @ConnectWifiByBle：WiFi连接失败，恢复设备状态为配网中", GetTimeString().c_str());
        Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
    }
}

void WifiBoard::StartNetwork() {
    ESP_LOGI(TAG, "%s @StartNetwork：开始启动网络", GetTimeString().c_str()); // 添加日志

    // 检查 NVS 中是否有 WiFi 配置
    ESP_LOGI(TAG, "%s @StartNetwork：检查 NVS 中的 WiFi 凭据...", GetTimeString().c_str());
    auto& ssid_manager = SsidManager::GetInstance();    // 获取 SSID 管理器实例
    auto ssid_list = ssid_manager.GetSsidList();        // 获取 SSID 列表
    bool nvs_is_empty = ssid_list.empty();              // 检查 NVS 是否为空

    ESP_LOGI(TAG, "%s @StartNetwork：NVS 中 SSID 数量: %d", GetTimeString().c_str(), ssid_list.size()); // 添加日志，显示 SSID 数量

    // 检查是否强制进入配网模式 (例如长按按钮触发)
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "%s @StartNetwork：检测到强制配网标志，直接进入配网模式", GetTimeString().c_str());
        EnterWifiConfigMode(); // 进入 BLE 配网模式
        return; // 不再继续尝试连接
    }

    // 如果 NVS 为空，进入 BLE 配网模式
    bool connected = false;
    // 如果 NVS 非空，尝试连接已保存的 WiFi
    if (!nvs_is_empty) {
        ESP_LOGI(TAG, "%s @StartNetwork：NVS 非空，尝试连接已保存的 WiFi...", GetTimeString().c_str());
        auto& wifi_station = WifiStation::GetInstance();
 
        // 设置 WiFi 扫描开始回调
        wifi_station.OnScanBegin([this]() {
            ESP_LOGI(TAG, "%s @StartNetwork.OnScanBegin：WiFi 扫描开始", GetTimeString().c_str());
            auto display = Board::GetInstance().GetDisplay();   // 获取显示实例
            if (display) display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000); // 显示扫描提示，持续 30 秒
        });

        // 设置 WiFi 连接开始回调
        wifi_station.OnConnect([this](const std::string& ssid) {
            ESP_LOGI(TAG, "%s @StartNetwork.OnConnect：开始连接 WiFi: %s", GetTimeString().c_str(), ssid.c_str());
            auto display = Board::GetInstance().GetDisplay();   // 获取显示实例
            if (display) {
                std::string notification = Lang::Strings::CONNECT_TO;    // 连接提示
                notification += ssid;    // 添加 SSID
                notification += "...";   // 添加连接中
                display->ShowNotification(notification.c_str(), 30000); // 显示连接提示，持续 30 秒
            }
        });

        // 设置 WiFi 连接成功回调
        wifi_station.OnConnected([this](const std::string& ssid) {
            ESP_LOGI(TAG, "%s @StartNetwork.OnConnected：WiFi 连接成功: %s", GetTimeString().c_str(), ssid.c_str());
            auto display = Board::GetInstance().GetDisplay();
            if (display) { // 添加判空
                std::string notification = Lang::Strings::CONNECTED_TO;
                notification += ssid;
                display->ShowNotification(notification.c_str(), 30000);
            }
        });

        wifi_station.Start();   /// <--- 启动 WiFi 连接
        // 等待连接结果
        ESP_LOGI(TAG, "%s @StartNetwork：等待 WiFi 连接，超时时间: 60 秒", GetTimeString().c_str());
        connected = wifi_station.WaitForConnected(60 * 1000);
        // 检查连接结果
        if (connected) {
            ESP_LOGI(TAG, "%s @StartNetwork：WiFi 连接成功，IP: %s", GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
            // --- 连接成功，正常启动 ---
            return; // 正常启动，不进入配网
        } else {
            ESP_LOGW(TAG, "%s @StartNetwork：尝试连接已保存的 WiFi 失败。", GetTimeString().c_str());
            wifi_station.Stop(); // 停止尝试连接

            // >>>>> 修改点: 如果尝试连接已保存的 Wi-Fi 失败，显式进入配网模式 <<<<<
            wifi_config_mode_ = true; // 标记进入配网模式，以便 EnterWifiConfigMode 执行
        }
    }

    // --- 执行到这里，说明：NVS 为空 或 NVS 非空但连接失败 ---
    ESP_LOGI(TAG, "%s @StartNetwork：需要进入配网模式 (NVS 是否为空: %s, 连接是否成功: %s)", GetTimeString().c_str(), nvs_is_empty ? "是" : "否", connected ? "是" : "否");

    // 播放配网提示音：
    auto& application = Application::GetInstance();
    ESP_LOGI(TAG, "%s @StartNetwork：播放配网提示音", GetTimeString().c_str());
    application.PlaySound(Lang::Sounds::P3_WIFI_CONFIG_REQUIRED);
    vTaskDelay(pdMS_TO_TICKS(500)); // 稍作延时

    // 进入 BLE 配网模式
    wifi_config_mode_ = true;   // 标记进入配网模式
    EnterWifiConfigMode();      // 进入 BLE 配网模式
}

Http* WifiBoard::CreateHttp() {
    ESP_LOGD(TAG, "@CreateHttp：创建 HTTP 客户端");
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    ESP_LOGD(TAG, "@CreateWebSocket：创建 WebSocket 客户端");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    std::string url = CONFIG_WEBSOCKET_URL;
    ESP_LOGI(TAG, "%s @CreateWebSocket：WebSocket URL: %s", GetTimeString().c_str(), url.c_str());
    if (url.find("wss://") == 0) {
        ESP_LOGI(TAG, "%s @CreateWebSocket：使用 TLS 传输层创建安全 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TlsTransport());
    } else {
        ESP_LOGI(TAG, "%s @CreateWebSocket：使用 TCP 传输层创建普通 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TcpTransport());
    }
#endif
    ESP_LOGW(TAG, "@CreateWebSocket：WebSocket 未配置，返回 nullptr");
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    ESP_LOGD(TAG, "@CreateMqtt：创建 MQTT 客户端");
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    ESP_LOGD(TAG, "@CreateUdp：创建 UDP 客户端");
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 配网模式");
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 未连接");
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    ESP_LOGD(TAG, "@GetNetworkStateIcon：网络状态: 已连接，信号强度: %d dBm", rssi);
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    ESP_LOGD(TAG, "@GetBoardJson：获取板卡 JSON 信息");
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\"" + wifi_station.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_station.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_station.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_station.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    ESP_LOGD(TAG, "@GetBoardJson：板卡 JSON: %s", board_json.c_str());
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    ESP_LOGI(TAG, "%s @SetPowerSaveMode：设置 WiFi 省电模式: %s", GetTimeString().c_str(), enabled ? "启用" : "禁用");
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
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
