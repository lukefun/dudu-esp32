#ifndef WATCHDOG_MONITOR_H
#define WATCHDOG_MONITOR_H

#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_log.h>
#else
#error "This code is intended to run on ESP32 platform only"
#endif

#include <string>
#include <stdint.h>

// 前向声明
static const char* WATCHDOG_TAG = "WatchdogMonitor";

class WatchdogMonitor {
public:
    static esp_err_t init(uint32_t timeout_ms = 30000) {
        if (timeout_ms < 1000 || timeout_ms > 300000) { // 1秒到5分钟的有效范围
            ESP_LOGE(WATCHDOG_TAG, "Invalid timeout value: %u ms (valid range: 1000-300000)", timeout_ms);
            return ESP_ERR_INVALID_ARG;
        }
        
        timeout_ = timeout_ms;
        last_feed_ = esp_timer_get_time() / 1000;  // 转换为毫秒
        return ESP_OK;
    }
    
    static esp_err_t feed() {
        esp_err_t err = esp_task_wdt_reset();  // 实际喂狗
        if (err != ESP_OK) {
            ESP_LOGW(WATCHDOG_TAG, "Failed to reset watchdog timer: %d", err);
            return err;
        }
        
        last_feed_ = esp_timer_get_time() / 1000;  // 记录喂狗时间
        return ESP_OK;
    }
    
    static int32_t get_remaining_time() {
        if (timeout_ == 0) {
            ESP_LOGW(WATCHDOG_TAG, "Watchdog not initialized");
            return -1;
        }
        
        int64_t now = esp_timer_get_time() / 1000;
        int32_t elapsed = (int32_t)(now - last_feed_);
        return (timeout_ - elapsed);
    }

    static void log_remaining_time(const char* tag, const char* operation) {
        if (!tag || !operation) {
            ESP_LOGW(WATCHDOG_TAG, "Invalid parameters (tag or operation is null)");
            return;
        }
        
        int32_t remaining = get_remaining_time();
        if (remaining < 0) {
            ESP_LOGE(tag, "[看门狗错误] %s，看门狗未初始化", operation);
        } else if (remaining < 5000) { // 剩余不足5秒时警告
            ESP_LOGW(tag, "[看门狗警告] %s，剩余时间严重不足:%dms", operation, remaining);
        } else {
            ESP_LOGI(tag, "[看门狗] %s，剩余时间:%dms", operation, remaining);
        }
    }

    // 新增：检查看门狗是否已初始化
    static bool is_initialized() {
        return timeout_ > 0;
    }

private:
    static int64_t last_feed_;
    static uint32_t timeout_;  // 0表示未初始化
};

#endif // WATCHDOG_MONITOR_H 