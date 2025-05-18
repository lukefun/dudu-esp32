#include "ble_task_state.h"

// 确保包含所有必要的头文件
#include "sdkconfig.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_log.h>
#include <sys/time.h>
#include <string>
#include <cstring>

// 获取当前时间字符串，包含毫秒
std::string GetTimeString() {
    // 使用系统时间（RTC）获取当前时间
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "[%H:%M:%S", tm_info);
    int len = strlen(buffer);
    snprintf(buffer + len, sizeof(buffer) - len, ".%03ld]", tv.tv_usec / 1000);
    return std::string(buffer);
}

// 获取当前内存状态
MemorySnapshot get_memory_snapshot() {
    // 使用ESP-IDF API获取内存信息
    MemorySnapshot snapshot;
    
    // 获取内部RAM
    snapshot.internal_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    
    // 获取总堆内存
    snapshot.total_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    
    // 获取最小剩余堆内存
    snapshot.min_heap = esp_get_minimum_free_heap_size();
    
    // 获取PSRAM
    snapshot.psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    // 获取DMA可用内存
    snapshot.dma_capable = heap_caps_get_free_size(MALLOC_CAP_DMA);
    
    // 获取可执行内存
    snapshot.exec_capable = heap_caps_get_free_size(MALLOC_CAP_EXEC);
    
    return snapshot;
}

// 打印内存状态日志
void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot, int log_level) {
    // 根据日志级别选择不同的日志函数
    switch (log_level) {
        case 0: // 错误
            ESP_LOGE(tag, "%s @log_memory_state - %s: 内存状态 - 内部RAM: %zu字节(%.1f%%), "
                         "总堆内存: %zu字节, 最小剩余堆内存: %zu字节, "
                         "PSRAM: %zu字节, DMA可用: %zu字节, 可执行: %zu字节", 
                     GetTimeString().c_str(), stage, 
                     snapshot.internal_ram, snapshot.GetInternalRamUsage(),
                     snapshot.total_heap, snapshot.min_heap,
                     snapshot.psram, snapshot.dma_capable, snapshot.exec_capable);
            break;
        case 1: // 警告
            ESP_LOGW(tag, "%s @log_memory_state - %s: 内存状态 - 内部RAM: %zu字节(%.1f%%), "
                         "总堆内存: %zu字节, 最小剩余堆内存: %zu字节, "
                         "PSRAM: %zu字节, DMA可用: %zu字节, 可执行: %zu字节", 
                     GetTimeString().c_str(), stage, 
                     snapshot.internal_ram, snapshot.GetInternalRamUsage(),
                     snapshot.total_heap, snapshot.min_heap,
                     snapshot.psram, snapshot.dma_capable, snapshot.exec_capable);
            break;
        case 3: // 调试
            ESP_LOGD(tag, "%s @log_memory_state - %s: 内存状态 - 内部RAM: %zu字节(%.1f%%), "
                         "总堆内存: %zu字节, 最小剩余堆内存: %zu字节, "
                         "PSRAM: %zu字节, DMA可用: %zu字节, 可执行: %zu字节", 
                     GetTimeString().c_str(), stage, 
                     snapshot.internal_ram, snapshot.GetInternalRamUsage(),
                     snapshot.total_heap, snapshot.min_heap,
                     snapshot.psram, snapshot.dma_capable, snapshot.exec_capable);
            break;
        case 4: // 详细
            ESP_LOGV(tag, "%s @log_memory_state - %s: 内存状态 - 内部RAM: %zu字节(%.1f%%), "
                         "总堆内存: %zu字节, 最小剩余堆内存: %zu字节, "
                         "PSRAM: %zu字节, DMA可用: %zu字节, 可执行: %zu字节", 
                     GetTimeString().c_str(), stage, 
                     snapshot.internal_ram, snapshot.GetInternalRamUsage(),
                     snapshot.total_heap, snapshot.min_heap,
                     snapshot.psram, snapshot.dma_capable, snapshot.exec_capable);
            break;
        case 2: // 信息 (默认)
        default:
            ESP_LOGI(tag, "%s @log_memory_state - %s: 内存状态 - 内部RAM: %zu字节(%.1f%%), "
                         "总堆内存: %zu字节, 最小剩余堆内存: %zu字节, "
                         "PSRAM: %zu字节, DMA可用: %zu字节, 可执行: %zu字节", 
                     GetTimeString().c_str(), stage, 
                     snapshot.internal_ram, snapshot.GetInternalRamUsage(),
                     snapshot.total_heap, snapshot.min_heap,
                     snapshot.psram, snapshot.dma_capable, snapshot.exec_capable);
            break;
    }
}

// 检查内存健康状态
bool check_memory_health(const char* tag, const char* stage, 
                         size_t internal_threshold, 
                         size_t total_threshold) {
    MemorySnapshot snapshot = get_memory_snapshot();
    
    bool is_healthy = true;
    
    // 检查内部RAM
    if (snapshot.internal_ram < internal_threshold) {
        ESP_LOGW(tag, "%s @check_memory_health - %s: 内部RAM不足! 仅剩 %zu 字节 (阈值: %zu 字节)",
                 GetTimeString().c_str(), stage, snapshot.internal_ram, internal_threshold);
        is_healthy = false;
    }
    
    // 检查总堆内存
    if (snapshot.total_heap < total_threshold) {
        ESP_LOGW(tag, "%s @check_memory_health - %s: 总堆内存不足! 仅剩 %zu 字节 (阈值: %zu 字节)",
                 GetTimeString().c_str(), stage, snapshot.total_heap, total_threshold);
        is_healthy = false;
    }
    
    // 如果内存健康，记录信息级别日志
    if (is_healthy) {
        ESP_LOGI(tag, "%s @check_memory_health - %s: 内存状态良好 - %s",
                 GetTimeString().c_str(), stage, snapshot.GetBriefDescription().c_str());
    } else {
        // 如果内存不健康，记录详细的内存状态
        log_memory_state(tag, stage, snapshot, 1); // 使用警告级别
    }
    
    return is_healthy;
}