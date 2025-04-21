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

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>

static const char *TAG = "WifiBoard";    // <--- 确保 TAG 已定义

WifiBoard::WifiBoard() {
    ESP_LOGI(TAG, "初始化 WifiBoard");
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "检测到强制配网标志 force_ap=1，重置为0");
        settings.SetInt("force_ap", 0);
    }
    ESP_LOGI(TAG, "WifiBoard 初始化完成，配网模式状态: %s", wifi_config_mode_ ? "启用" : "禁用");
}

std::string WifiBoard::GetBoardType() {
    ESP_LOGD(TAG, "获取板卡类型: wifi");
    return "wifi";
}

// 修改 EnterWifiConfigMode 函数，添加 BLE 配网初始化
void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "进入 WiFi 配网模式");
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    ESP_LOGI(TAG, "设备状态已设置为: WiFi配网中");

    // 初始化并启动 BLE 配网
    ESP_LOGI(TAG, "正在初始化 BLE 配网...");
    auto& ble_config = BleConfig::GetInstance();
    
    // 设置 WiFi 凭据接收回调
    ESP_LOGI(TAG, "设置 WiFi 凭据接收回调");
    ble_config.SetCredentialsReceivedCallback([](const std::string& ssid, const std::string& password) {
        ESP_LOGI(TAG, "收到 WiFi 凭据 - SSID: %s, 密码长度: %d", ssid.c_str(), password.length());
        
        // 保存 WiFi 凭据
        ESP_LOGI(TAG, "正在保存 WiFi 凭据到 SSID 管理器");
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(ssid, password);
        ESP_LOGI(TAG, "WiFi 凭据保存成功");
        
        // 发送成功状态
        ESP_LOGI(TAG, "发送 WiFi 配置成功状态到客户端");
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_SUCCESS);
        
        // 延迟一段时间后重启设备
        ESP_LOGI(TAG, "将在 2 秒后重启设备以应用新的 WiFi 配置");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    });
    
    // 设置开始连接 WiFi 的回调
    ESP_LOGI(TAG, "设置连接 WiFi 回调");
    ble_config.SetConnectWifiCallback([]() {
        ESP_LOGI(TAG, "收到连接 WiFi 命令");
        ESP_LOGI(TAG, "发送 WiFi 连接中状态到客户端");
        BleConfig::GetInstance().SendWifiStatus(WIFI_STATUS_CONNECTING);
        
        // 这里可以添加连接 WiFi 的逻辑，或者直接重启设备让它自动连接
        ESP_LOGI(TAG, "将在 1 秒后重启设备以连接 WiFi");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    });
    
    // 初始化 BLE
    ESP_LOGI(TAG, "开始初始化 BLE 模块");
    ble_config.Initialize();
    ESP_LOGI(TAG, "BLE 模块初始化完成");
    
    // 开始广播
    ESP_LOGI(TAG, "开始 BLE 广播");

    if (!ble_config.StartAdvertising()) {
        ESP_LOGE(TAG, "BLE广播启动失败");
        // 如果有 Lang::Sounds::P3_ERROR 就用，否则用 ""
        application.Alert("蓝牙配网启动失败", "请重启设备", "", Lang::Sounds::P3_ERR_PIN); // 临时替代一下
        return;
    }
    ESP_LOGI(TAG, "BLE 配网已启动，等待连接...");

    // 显示 BLE 配网提示
    ESP_LOGI(TAG, "准备显示 BLE 配网提示");
    std::string hint = Lang::Strings::CONNECT_TO_BLE;  // 需要确认这个字符串存在
    if (hint.empty()) {
        ESP_LOGW(TAG, "未找到 CONNECT_TO_BLE 字符串，使用默认提示");
        hint = "请使用支持BLE的手机App扫描并连接设备：";
    }
    hint += "DuDu-BLE";  // 使用BLE设备名称
    ESP_LOGI(TAG, "配网提示: %s", hint.c_str());
    
    // 播报配置 WiFi 的提示
    ESP_LOGI(TAG, "显示配网提示并播放提示音");
    application.Alert(Lang::Strings::BLE_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);
    
    // Wait forever until reset after configuration
    ESP_LOGI(TAG, "进入配网等待循环，等待配网完成或设备重启");
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "内存状态 - 当前可用: %u 字节, 最小可用: %u 字节", free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    ESP_LOGI(TAG, "开始启动网络");

    auto& application = Application::GetInstance(); // <--- 获取 Application 实例
    // auto display = Board::GetInstance().GetDisplay(); // <--- 获取 Display 实例:后面没有按照AI的指导来改，有可能多余了，先留着。
    
    // User can press BOOT button while starting to enter WiFi configuration mode
    // 用户可以在启动时按下 BOOT 按钮进入 WiFi 配置模式。
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "检测到配网模式标志，直接进入配网模式");
        // 如果是强制进入AP模式，这里可能不需要播放提示音，直接进入配网
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    ESP_LOGI(TAG, "检查是否已配置 WiFi 凭据");
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "未找到 WiFi 凭据，需要进入配网模式");
        // 播放配网提示音
        ESP_LOGI(TAG, "播放配网提示音");
        application.PlaySound(Lang::Sounds::P3_WIFI_CONFIG_REQUIRED);// <--- 播放音频指引扫码配网
        vTaskDelay(pdMS_TO_TICKS(500));// 等待一小段时间，确保音频开始播放

        wifi_config_mode_ = true;
        ESP_LOGI(TAG, "进入 BLE 配网模式");
        EnterWifiConfigMode();
        return;
    }

    ESP_LOGI(TAG, "找到 WiFi 凭据，尝试连接 WiFi");
    auto& wifi_station = WifiStation::GetInstance();
    
    ESP_LOGI(TAG, "设置 WiFi 扫描开始回调");
    wifi_station.OnScanBegin([this]() {
        ESP_LOGI(TAG, "WiFi 扫描开始");
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    
    ESP_LOGI(TAG, "设置 WiFi 连接开始回调");
    wifi_station.OnConnect([this](const std::string& ssid) {
        ESP_LOGI(TAG, "开始连接 WiFi: %s", ssid.c_str());
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    
    ESP_LOGI(TAG, "设置 WiFi 连接成功回调");
    wifi_station.OnConnected([this](const std::string& ssid) {
        ESP_LOGI(TAG, "WiFi 连接成功: %s", ssid.c_str());
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    
    ESP_LOGI(TAG, "启动 WiFi 站点模式");
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    ESP_LOGI(TAG, "等待 WiFi 连接，超时时间: 60 秒");
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        // --- 连接失败逻辑 ---
        // 这里可以添加播放连接失败的提示音（如果需要）
        ESP_LOGW(TAG, "无法连接到无线网络。正在进入配置模式。 Failed to connect to Wi-Fi. Entering config mode.");
        ESP_LOGI(TAG, "停止 WiFi 站点模式");
        wifi_station.Stop();
        wifi_config_mode_ = true;
        ESP_LOGI(TAG, "进入 BLE 配网模式");
        EnterWifiConfigMode();
        return;
    }
    // --- 连接成功，正常启动 ---
    ESP_LOGI(TAG, "WiFi 连接成功，IP: %s", wifi_station.GetIpAddress().c_str());
}

Http* WifiBoard::CreateHttp() {
    ESP_LOGD(TAG, "创建 HTTP 客户端");
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    ESP_LOGD(TAG, "创建 WebSocket 客户端");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    std::string url = CONFIG_WEBSOCKET_URL;
    ESP_LOGI(TAG, "WebSocket URL: %s", url.c_str());
    if (url.find("wss://") == 0) {
        ESP_LOGI(TAG, "使用 TLS 传输层创建安全 WebSocket");
        return new WebSocket(new TlsTransport());
    } else {
        ESP_LOGI(TAG, "使用 TCP 传输层创建普通 WebSocket");
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
    ESP_LOGI(TAG, "设置 WiFi 省电模式: %s", enabled ? "启用" : "禁用");
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    ESP_LOGI(TAG, "重置 WiFi 配置");
    // Set a flag and reboot the device to enter the network configuration mode
    {
        ESP_LOGI(TAG, "设置强制配网标志 force_ap=1");
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    ESP_LOGI(TAG, "显示进入配网模式提示");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    ESP_LOGI(TAG, "等待 1 秒后重启设备");
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    ESP_LOGI(TAG, "重启设备以进入配网模式");
    esp_restart();
}
