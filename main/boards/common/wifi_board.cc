#include "wifi_board.h"

#include "display.h"
#include "application.h"    // <--- 包含 Application 头文件
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h" // <--- 包含语言配置头文件
#include "board.h"              // <--- 包含 Board 头文件
// 添加 BLE 配网头文件
#include "ble_config/ble_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>
#include "esp_task_wdt.h"

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include <sys/time.h>

static const char *TAG = "WifiBoard";    // <--- 确保 TAG 已定义

// 获取当前时间字符串
static std::string GetTimeString() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "[%H:%M:%S]", tm_info);
    return std::string(buffer);
}

WifiBoard::WifiBoard() {
    ESP_LOGI(TAG, "%s 初始化 WifiBoard", GetTimeString().c_str());
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "%s 检测到强制配网标志 force_ap=1，重置为0", GetTimeString().c_str());
        settings.SetInt("force_ap", 0);
    }
    ESP_LOGI(TAG, "%s WifiBoard 初始化完成，配网模式状态: %s", GetTimeString().c_str(), wifi_config_mode_ ? "启用" : "禁用");
}

std::string WifiBoard::GetBoardType() {
    ESP_LOGD(TAG, "获取板卡类型: wifi");
    return "wifi";
}

// 修改 EnterWifiConfigMode 函数，添加 BLE 配网初始化
void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "%s 进入 WiFi 配网模式", GetTimeString().c_str());

    // 设置设备状态为 WiFi配网中
    auto& application = Application::GetInstance();
    
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    ESP_LOGI(TAG, "%s 设备状态已设置为: WiFi配网中", GetTimeString().c_str());

    // 初始化并启动 BLE 配网
    auto& ble_config = BleConfig::GetInstance();
    
    // 设置 WiFi 凭据接收回调
    ESP_LOGI(TAG, "%s 设置 WiFi 凭据接收回调", GetTimeString().c_str());
    ble_config.SetCredentialsReceivedCallback([this](const std::string& ssid, const std::string& password) {
        ESP_LOGI(TAG, "%s 收到 WiFi 凭据 - SSID: %s, 密码长度: %d", GetTimeString().c_str(), ssid.c_str(), password.length());
        ble_ssid_ = ssid;
        ble_password_ = password;
        ESP_LOGI(TAG, "%s WiFi 凭据已暂存，等待连接命令", GetTimeString().c_str());
        // 不直接保存和重启，等收到连接命令后再处理
    });

    // 设置开始连接 WiFi 的回调
    ESP_LOGI(TAG, "%s 设置连接 WiFi 回调", GetTimeString().c_str());
    ble_config.SetConnectWifiCallback([this]() {
        ESP_LOGI(TAG, "%s 收到连接 WiFi 命令", GetTimeString().c_str());
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTING);

        // 检查凭据
        if (ble_ssid_.empty() || ble_password_.empty()) {
            ESP_LOGW(TAG, "BLE配网凭据为空，无法连接");
            BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_FAIL_SSID);
            return;
        }
        // 优化：收到连接命令后，先停止BLE广播，延迟一段时间再启动WiFi，避免任务高峰重叠
        BleConfig::GetInstance().StopAdvertising();
        ESP_LOGI(TAG, "%s 已停止BLE广播，准备延迟启动WiFi", GetTimeString().c_str());
        vTaskDelay(pdMS_TO_TICKS(500)); // 延迟500ms，确保BLE主机任务空闲
        ConnectWifiByBle(ble_ssid_, ble_password_);
    });

    // 初始化 BLE
    ESP_LOGI(TAG, "%s 开始初始化 BLE 模块", GetTimeString().c_str());
    ble_config.Initialize();
    ESP_LOGI(TAG, "%s BLE 模块初始化完成", GetTimeString().c_str());

    // 开始广播
    ESP_LOGI(TAG, "%s 开始 BLE 广播", GetTimeString().c_str());
    if (!ble_config.StartAdvertising()) {
        ESP_LOGE(TAG, "BLE广播启动失败");
        application.Alert("蓝牙配网启动失败", "请重启设备", "", Lang::Sounds::P3_ERR_PIN);
        return;
    }
    ESP_LOGI(TAG, "%s BLE 配网已启动，等待连接...", GetTimeString().c_str());

    // 显示 BLE 配网提示
    ESP_LOGI(TAG, "%s 准备显示 BLE 配网提示", GetTimeString().c_str());
    std::string hint = Lang::Strings::CONNECT_TO_BLE;
    if (hint.empty()) {
        ESP_LOGW(TAG, "未找到 CONNECT_TO_BLE 字符串，使用默认提示");
        hint = "请使用支持BLE的手机App扫描并连接设备：";
    }
    hint += "DuDu-BLE"; // 确保与BLE广播名称一致
    ESP_LOGI(TAG, "%s 配网提示: %s", GetTimeString().c_str(), hint.c_str());

    ESP_LOGI(TAG, "%s 显示配网提示并播放提示音", GetTimeString().c_str());
    application.Alert(Lang::Strings::BLE_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);

    ESP_LOGI(TAG, "%s 进入配网等待循环，等待配网完成或设备重启", GetTimeString().c_str());
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "%s 内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", GetTimeString().c_str(), free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// 新增：BLE配网流程中的WiFi连接实现 - 优化内存使用，避免栈溢出
void WifiBoard::ConnectWifiByBle(const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "%s BLE配网流程：准备连接WiFi SSID: %s", GetTimeString().c_str(), ssid.c_str());
    
    // 检查当前内存状态
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s 连接WiFi前内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", 
             GetTimeString().c_str(), free_sram, min_free_sram);
    
    // 先保存WiFi凭据到NVS
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
    
    // 短暂延时，让BLE任务有时间处理完成当前操作
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 添加认证信息
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.AddAuth(std::string(ssid), std::string(password));
    
    // 再次检查内存状态
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s 启动WiFi前内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", 
             GetTimeString().c_str(), free_sram, min_free_sram);
    
    // 如果可用内存过低，尝试释放一些内存
    if (free_sram < 60000) {
        ESP_LOGW(TAG, "%s 可用内存较低，尝试释放资源...", GetTimeString().c_str());
        // 停止BLE广播以释放资源
        BleConfig::GetInstance().StopAdvertising();
        // 强制执行垃圾回收
        heap_caps_check_integrity_all(true);
        // 再次检查内存
        free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "%s 释放资源后内存状态 - 当前可用: %u 字节", GetTimeString().c_str(), free_sram);
    }
    
    // 重置任务看门狗，防止WiFi初始化过程中触发看门狗超时
    esp_task_wdt_reset();
    
    // 启动WiFi
    wifi_station.Start();
    
    // 短暂延时，让WiFi任务有时间初始化
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 重置任务看门狗
    esp_task_wdt_reset();
    
    // 等待连接结果，使用更长的超时时间
    ESP_LOGI(TAG, "%s 等待WiFi连接结果，超时时间: 20秒", GetTimeString().c_str());
    bool connected = wifi_station.WaitForConnected(20000); // 增加到20秒

    // 连接过程完成，再次重置看门狗
    esp_task_wdt_reset();

    if (connected) {
        ESP_LOGI(TAG, "%s WiFi连接成功，IP: %s", GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
        // 通知BLE配网结果
        OnBleWifiConnectResult(true, 0);
    } else {
        ESP_LOGW(TAG, "%s WiFi连接失败", GetTimeString().c_str());
        // 通知BLE配网结果，使用自定义错误码
        OnBleWifiConnectResult(false, BLE_WIFI_REASON_CONNECTION_FAIL);
    } // 连接失败，继续等待用户操作或重试
}

// 新增：BLE配网错误码
enum {
    // 删除重复的枚举定义
    // BLE_WIFI_REASON_AUTH_FAIL = 1,
    // BLE_WIFI_REASON_NO_AP_FOUND,
    // BLE_WIFI_REASON_CONNECTION_FAIL,
    BLE_WIFI_REASON_INIT_FAIL,     // WiFi初始化失败
};

// 新增：处理BLE配网结果
void WifiBoard::OnBleWifiConnectResult(bool success, int err_code) {
    if (success) {
        ESP_LOGI(TAG, "%s BLE配网WiFi连接成功", GetTimeString().c_str());
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTED);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "BLE配网WiFi连接失败，错误码: %d", err_code);
        // 根据错误码发送不同的状态
        wifi_config_status_t status;
        switch (err_code) {
            case BLE_WIFI_REASON_AUTH_FAIL:
                status = WIFI_STATUS_FAIL_AUTH;
                break;
            case BLE_WIFI_REASON_NO_AP_FOUND:
                status = WIFI_STATUS_FAIL_AP_NOT_FOUND;
                break;
            case BLE_WIFI_REASON_CONNECTION_FAIL:
                status = WIFI_STATUS_FAIL_CONN;
                break;
            default:
                status = WIFI_STATUS_FAIL_OTHER;
                break;
        }
        BleConfig::GetInstance().SendWifiStatus(status);
        // 可根据需要决定是否重启或等待用户重新输入
    }
}

void WifiBoard::StartNetwork() {

    auto& application = Application::GetInstance();
    ESP_LOGI(TAG, "%s 开始启动网络", GetTimeString().c_str()); // 添加日志

    // 检查 NVS 中是否有 WiFi 配置
    ESP_LOGI(TAG, "%s 检查 NVS 中的 WiFi 凭据...", GetTimeString().c_str());
    auto& ssid_manager = SsidManager::GetInstance();    // <--- 确保 SsidManager 已定义
    auto ssid_list = ssid_manager.GetSsidList();        
    bool nvs_is_empty = ssid_list.empty();              // 记录 NVS 是否为空

    ESP_LOGI(TAG, "%s NVS 中 SSID 数量: %d", GetTimeString().c_str(), ssid_list.size()); // 添加日志，显示 SSID 数量

    // 检查是否强制进入配网模式 (例如长按按钮触发)
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "%s 检测到强制配网标志，直接进入配网模式", GetTimeString().c_str());
        EnterWifiConfigMode(); // 进入 BLE 配网模式
        return; // 不再继续尝试连接
    }

    // 如果 NVS 不为空，尝试连接 WiFi
    bool connected = false;
    if (!nvs_is_empty) {
        ESP_LOGI(TAG, "%s NVS 非空，尝试连接已保存的 WiFi...", GetTimeString().c_str());
        auto& wifi_station = WifiStation::GetInstance();
 
        // ... (保留原有的 OnScanBegin, OnConnect, OnConnected 回调设置) ...
        // 设置 WiFi 扫描开始回调
        wifi_station.OnScanBegin([this]() {
            ESP_LOGI(TAG, "%s WiFi 扫描开始", GetTimeString().c_str());
            auto display = Board::GetInstance().GetDisplay();
            if (display) display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000); // 添加判空
        });

        // 设置 WiFi 连接开始回调
        wifi_station.OnConnect([this](const std::string& ssid) {
            ESP_LOGI(TAG, "%s 开始连接 WiFi: %s", GetTimeString().c_str(), ssid.c_str());
            auto display = Board::GetInstance().GetDisplay();
            if (display) { // 添加判空
                std::string notification = Lang::Strings::CONNECT_TO;
                notification += ssid;
                notification += "...";
                display->ShowNotification(notification.c_str(), 30000);
            }
        });

        // 设置 WiFi 连接成功回调
        wifi_station.OnConnected([this](const std::string& ssid) {
            ESP_LOGI(TAG, "%s WiFi 连接成功: %s", GetTimeString().c_str(), ssid.c_str());
            auto display = Board::GetInstance().GetDisplay();
            if (display) { // 添加判空
                std::string notification = Lang::Strings::CONNECTED_TO;
                notification += ssid;
                display->ShowNotification(notification.c_str(), 30000);
            }
        });


        wifi_station.Start();   /// <--- 启动 WiFi 连接
        // 等待连接结果
        ESP_LOGI(TAG, "%s 等待 WiFi 连接，超时时间: 60 秒", GetTimeString().c_str());
        connected = wifi_station.WaitForConnected(60 * 1000);
        // 检查连接结果
        if (connected) {
            ESP_LOGI(TAG, "%s WiFi 连接成功，IP: %s", GetTimeString().c_str(), wifi_station.GetIpAddress().c_str());
            // --- 连接成功，正常启动 ---
            return; // 正常启动，不进入配网
        } else {
            ESP_LOGW(TAG, "尝试连接已保存的 WiFi 失败。");
            wifi_station.Stop(); // 停止尝试连接
        }
    }

    // --- 执行到这里，说明：NVS 为空 或 NVS 非空但连接失败 ---
    ESP_LOGI(TAG, "%s 需要进入配网模式 (NVS 是否为空: %s, 连接是否成功: %s)", GetTimeString().c_str(), nvs_is_empty ? "是" : "否", connected ? "是" : "否");

    // 仅在 NVS *完全为空* 时播放提示音
    if (nvs_is_empty) {
        ESP_LOGI(TAG, "%s NVS 为空，播放配网提示音", GetTimeString().c_str());
        application.PlaySound(Lang::Sounds::P3_WIFI_CONFIG_REQUIRED);
        vTaskDelay(pdMS_TO_TICKS(500)); // 稍作延时
    } else {
        ESP_LOGI(TAG, "%s NVS 非空但连接失败，不播放初始提示音，直接进入配网", GetTimeString().c_str());
    }

    // 进入 BLE 配网模式
    wifi_config_mode_ = true; // 标记进入配网模式
    EnterWifiConfigMode();
}

Http* WifiBoard::CreateHttp() {
    ESP_LOGD(TAG, "创建 HTTP 客户端");
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    ESP_LOGD(TAG, "创建 WebSocket 客户端");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    std::string url = CONFIG_WEBSOCKET_URL;
    ESP_LOGI(TAG, "%s WebSocket URL: %s", GetTimeString().c_str(), url.c_str());
    if (url.find("wss://") == 0) {
        ESP_LOGI(TAG, "%s 使用 TLS 传输层创建安全 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TlsTransport());
    } else {
        ESP_LOGI(TAG, "%s 使用 TCP 传输层创建普通 WebSocket", GetTimeString().c_str());
        return new WebSocket(new TcpTransport());
    }
#endif
    ESP_LOGW(TAG, "WebSocket 未配置，返回 nullptr");
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    ESP_LOGD(TAG, "创建 MQTT 客户端");
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    ESP_LOGD(TAG, "创建 UDP 客户端");
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        ESP_LOGD(TAG, "网络状态: 配网模式");
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        ESP_LOGD(TAG, "网络状态: 未连接");
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    ESP_LOGD(TAG, "网络状态: 已连接，信号强度: %d dBm", rssi);
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    ESP_LOGD(TAG, "获取板卡 JSON 信息");
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
    ESP_LOGD(TAG, "板卡 JSON: %s", board_json.c_str());
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    ESP_LOGI(TAG, "%s 设置 WiFi 省电模式: %s", GetTimeString().c_str(), enabled ? "启用" : "禁用");
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    ESP_LOGI(TAG, "%s 重置 WiFi 配置", GetTimeString().c_str());
    // Set a flag and reboot the device to enter the network configuration mode
    {
        ESP_LOGI(TAG, "%s 设置强制配网标志 force_ap=1", GetTimeString().c_str());
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    ESP_LOGI(TAG, "%s 显示进入配网模式提示", GetTimeString().c_str());
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    ESP_LOGI(TAG, "%s 等待 1 秒后重启设备", GetTimeString().c_str());
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    ESP_LOGI(TAG, "%s 重启设备以进入配网模式", GetTimeString().c_str());
    esp_restart();
}
