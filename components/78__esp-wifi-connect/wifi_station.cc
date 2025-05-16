#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include <inttypes.h> // 引入 PRIu32 宏需要的头文件
#include "ssid_manager.h"

#include <esp_event.h>    // 事件 API


#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}


// ========== 完整替换 WifiStation::Stop() 函数 ==========
void WifiStation::Stop() {
    ESP_LOGI(TAG, "[%.3f] WifiStation::Stop() 被调用 / Stop() called.", (double)esp_log_timestamp() / 1000.0);

    // 检查是否需要停止（优化判断）
    if (!this->started_ && !this->default_netif_) {
        ESP_LOGI(TAG, "[%.3f] WifiStation 未启动且无 Netif，无需停止 / WifiStation not started and no netif, nothing to stop.", (double)esp_log_timestamp() / 1000.0);
        return;
    }

    // 标记为停止
    this->started_ = false;
    ESP_LOGI(TAG, "[%.3f] WifiStation 状态标记为停止 / WifiStation state marked as stopped.", (double)esp_log_timestamp() / 1000.0);

    ESP_LOGI(TAG, "[%.3f] 停止 Wi-Fi 操作 / Stopping Wi-Fi operations...", (double)esp_log_timestamp() / 1000.0);
    esp_err_t err;

    // 尝试断开连接
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "[%.3f] esp_wifi_disconnect 失败 / failed: %s (%d)", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(err), err);
    } else {
        ESP_LOGD(TAG, "[%.3f] Wi-Fi 断开命令已发出或不适用 / Wi-Fi disconnect command issued or not applicable.", (double)esp_log_timestamp() / 1000.0);
    }

    // 停止 Wi-Fi
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "[%.3f] esp_wifi_stop 失败 / failed: %s (%d)", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(err), err);
    } else {
        ESP_LOGD(TAG, "[%.3f] Wi-Fi 停止命令已发出或不适用 / Wi-Fi stop command issued or not applicable.", (double)esp_log_timestamp() / 1000.0);
    }

    // 注销事件处理器 (使用实例句柄)
    ESP_LOGI(TAG, "[%.3f] 注销 Wi-Fi 事件处理器 / Unregistering Wi-Fi event handlers...", (double)esp_log_timestamp() / 1000.0);
    if (this->instance_any_id_) { // 检查句柄是否存在
        err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, this->instance_any_id_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
           ESP_LOGW(TAG, "[%.3f] 注销 WIFI_EVENT 处理器失败 / Failed to unregister WIFI_EVENT handler: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(err));
        } else {
           this->instance_any_id_ = nullptr; // 清空句柄
        }
    }
    if (this->instance_got_ip_) { // 检查句柄是否存在
        err = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, this->instance_got_ip_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
           ESP_LOGW(TAG, "[%.3f] 注销 IP_EVENT 处理器失败 / Failed to unregister IP_EVENT handler: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(err));
        } else {
           this->instance_got_ip_ = nullptr; // 清空句柄
        }
    }

    // 反初始化 WiFi 驱动
    ESP_LOGI(TAG, "[%.3f] 反初始化 Wi-Fi 驱动 / Deinitializing Wi-Fi driver...", (double)esp_log_timestamp() / 1000.0);
    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "[%.3f] esp_wifi_deinit 失败 / failed: %s (%d)", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(err), err);
    } else {
        ESP_LOGI(TAG, "[%.3f] Wi-Fi 驱动已反初始化 / Wi-Fi driver deinitialized.", (double)esp_log_timestamp() / 1000.0);
    }

    // 销毁 Netif
    if (this->default_netif_) {
        ESP_LOGI(TAG, "[%.3f] 销毁默认 STA netif / Destroying default STA netif...", (double)esp_log_timestamp() / 1000.0);
        esp_netif_destroy(this->default_netif_);
        this->default_netif_ = nullptr; // 清空指针
        ESP_LOGI(TAG, "[%.3f] 默认 STA netif 已销毁 / Default STA netif destroyed.", (double)esp_log_timestamp() / 1000.0);
    } else {
        ESP_LOGW(TAG, "[%.3f] Stop() 时默认 STA netif 已为空 / Default STA netif was already null during Stop().", (double)esp_log_timestamp() / 1000.0);
    }

    // 清理状态变量
    this->ip_address_ = "";
    this->connected_ = false;
    this->auths_.clear();
    ESP_LOGI(TAG, "[%.3f] WiFi station 已停止并清理资源 / WiFi station stopped and resources cleaned.", (double)esp_log_timestamp() / 1000.0);
}


// ========== 完整替换 WifiStation::Start() 函数 ==========
void WifiStation::Start() {
    // ** 使用 %" PRIu32 " 打印 uint32_t **
    ESP_LOGI(TAG, "[%.3f] WifiStation::Start() 被调用 / called. 可用堆内存 / Free Heap: %" PRIu32 ", 最小可用堆内存 / Min Free Heap: %" PRIu32,
             (double)esp_log_timestamp() / 1000.0,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    if (this->started_) {
        ESP_LOGW(TAG, "[%.3f] WifiStation 已启动，忽略 Start() 调用 / WifiStation already started, ignoring Start().", (double)esp_log_timestamp() / 1000.0);
        return;
    }

    esp_err_t ret;

    // --- 检查并创建 Netif ---
    if (!this->default_netif_) {
        ESP_LOGI(TAG, "[%.3f] 创建默认 Wi-Fi STA netif... / Creating default Wi-Fi STA netif...", (double)esp_log_timestamp() / 1000.0);
        this->default_netif_ = esp_netif_create_default_wifi_sta();
        if (!this->default_netif_) {
            ESP_LOGE(TAG, "[%.3f] 创建默认 wifi sta netif 失败 / Failed to create default wifi sta netif", (double)esp_log_timestamp() / 1000.0);
            this->started_ = false;
            return;
        }
         ESP_LOGI(TAG, "[%.3f] 默认 Wi-Fi STA netif 已创建 / Default Wi-Fi STA netif created.", (double)esp_log_timestamp() / 1000.0);
    } else {
         ESP_LOGW(TAG, "[%.3f] 默认 Wi-Fi STA netif 已存在，重用 / Default Wi-Fi STA netif already exists, reusing.", (double)esp_log_timestamp() / 1000.0);
    }
    // ------------------------

    // --- 初始化 WiFi ---
    ESP_LOGI(TAG, "[%.3f] 初始化 Wi-Fi 驱动... / Initializing Wi-Fi driver...", (double)esp_log_timestamp() / 1000.0);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%.3f] esp_wifi_init 失败 / failed: %s (%d)", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret), ret);
        if (this->default_netif_) {
             esp_netif_destroy(this->default_netif_);
             this->default_netif_ = nullptr;
        }
        this->started_ = false;
        return;
    }
     ESP_LOGI(TAG, "[%.3f] Wi-Fi 驱动已初始化 / Wi-Fi driver initialized.", (double)esp_log_timestamp() / 1000.0);
    // -----------------

    // --- 注册事件处理器 ---
    ESP_LOGI(TAG, "[%.3f] 注册 Wi-Fi 事件处理器... / Registering Wi-Fi event handlers...", (double)esp_log_timestamp() / 1000.0);
    // ** 使用正确的静态成员函数指针和实例句柄 **
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler, this, &this->instance_any_id_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%.3f] 注册 WIFI_EVENT 处理器失败 / Failed to register WIFI_EVENT handler: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
        esp_wifi_deinit();
        if (this->default_netif_) { esp_netif_destroy(this->default_netif_); this->default_netif_ = nullptr; }
        this->started_ = false;
        return;
    }
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler, this, &this->instance_got_ip_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%.3f] 注册 IP_EVENT 处理器失败 / Failed to register IP_EVENT handler: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, this->instance_any_id_);
        this->instance_any_id_ = nullptr;
        esp_wifi_deinit();
        if (this->default_netif_) { esp_netif_destroy(this->default_netif_); this->default_netif_ = nullptr; }
        this->started_ = false;
        return;
    }
     ESP_LOGI(TAG, "[%.3f] Wi-Fi 事件处理器已注册 / Wi-Fi event handlers registered.", (double)esp_log_timestamp() / 1000.0);
    // ----------------------

    // --- 设置模式 ---
    ESP_LOGI(TAG, "[%.3f] 设置 Wi-Fi 模式为 STA... / Setting Wi-Fi mode to STA...", (double)esp_log_timestamp() / 1000.0);
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%.3f] esp_wifi_set_mode 失败 / failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
        Stop();
        return;
    }
     ESP_LOGI(TAG, "[%.3f] Wi-Fi 模式已设为 STA / Wi-Fi mode set to STA.", (double)esp_log_timestamp() / 1000.0);
    // ---------------

    // 标记为已启动（核心初始化完成后）
    this->started_ = true;

    // 如果有认证信息，尝试连接
    if (!this->auths_.empty()) {
        auto auth = this->auths_.front();
        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, auth.first.c_str(), sizeof(wifi_config.sta.ssid) - 1);
        wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
        strncpy((char*)wifi_config.sta.password, auth.second.c_str(), sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        // ** 使用 (const char*) 强制类型转换 SSID 用于日志打印 **
        ESP_LOGI(TAG, "[%.3f] 设置 Wi-Fi 配置，SSID: %s / Setting Wi-Fi configuration for SSID: %s", 
            (double)esp_log_timestamp() / 1000.0, (const char*)wifi_config.sta.ssid, (const char*)wifi_config.sta.ssid);

        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[%.3f] esp_wifi_set_config 失败 / failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
            Stop();
            return;
        }

        ESP_LOGI(TAG, "[%.3f] 启动 Wi-Fi... / Starting Wi-Fi...", (double)esp_log_timestamp() / 1000.0);
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[%.3f] esp_wifi_start 失败 / failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
            Stop();
            return;
        }

        // ** 使用 (const char*) 强制类型转换 SSID 用于日志打印 **
        ESP_LOGI(TAG, "[%.3f] 连接到 AP %s... / Connecting to AP ...", (double)esp_log_timestamp() / 1000.0, (const char*)wifi_config.sta.ssid);
        if (this->on_connect_) {
            this->on_connect_(auth.first);
        }
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[%.3f] esp_wifi_connect 失败 / failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
        }
         ESP_LOGI(TAG, "[%.3f] Wi-Fi 连接进程已启动 / Wi-Fi connection process initiated.", (double)esp_log_timestamp() / 1000.0);
    } else { // 否则开始扫描
         ESP_LOGI(TAG, "[%.3f] 无认证信息，开始扫描... / No auth info, start scanning...", (double)esp_log_timestamp() / 1000.0);
         if (this->on_scan_begin_) {
            this->on_scan_begin_();
         }
         ret = esp_wifi_start();
         if (ret != ESP_OK) {
             ESP_LOGE(TAG, "[%.3f] esp_wifi_start (扫描) 失败 / for scan failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
             Stop();
             return;
         }
         ret = esp_wifi_scan_start(NULL, false);
         if (ret != ESP_OK) {
             ESP_LOGE(TAG, "[%.3f] esp_wifi_scan_start 失败 / failed: %s", (double)esp_log_timestamp() / 1000.0, esp_err_to_name(ret));
             Stop();
             return;
         }
          ESP_LOGI(TAG, "[%.3f] Wi-Fi 扫描已启动 / Wi-Fi scan initiated.", (double)esp_log_timestamp() / 1000.0);
    }
     ESP_LOGI(TAG, "[%.3f] WifiStation::Start() 执行完毕 / finished.", (double)esp_log_timestamp() / 1000.0);
}


void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}



bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan");
        esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    wifi_config.sta.channel = ap_record.channel;
    memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
    wifi_config.sta.bssid_set = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    ESP_ERROR_CHECK(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        esp_timer_start_once(this_->timer_handle_, 10 * 1000);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}
