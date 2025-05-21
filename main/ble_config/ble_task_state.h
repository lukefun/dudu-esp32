#ifndef BLE_TASK_STATE_H
#define BLE_TASK_STATE_H

// 确保ESP-IDF头文件在最前面
#include "sdkconfig.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_log.h>

// 然后是标准库头文件
#include <sys/time.h>
#include <string>
#include <cstring>

// 显式定义可能缺失的宏
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL (1<<0)  // 内部内存
#endif

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT (1<<1)      // 8位可访问内存
#endif

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (1<<2)       // DMA可用内存
#endif

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM (1<<3)    // PSRAM内存
#endif

#ifndef MALLOC_CAP_EXEC
#define MALLOC_CAP_EXEC (1<<4)      // 可执行内存
#endif

// BLE任务状态枚举
typedef enum {
    BLE_TASK_INIT = 0,      // 任务初始化状态
    BLE_TASK_RUNNING = 1,   // 任务运行中状态
    BLE_TASK_STOPPING = 2,  // 任务正在停止状态
    BLE_TASK_STOPPED = 3    // 任务已停止状态
} ble_task_state_t;

// 获取时间字符串函数声明
std::string GetTimeString();

/**
 * @brief 内存快照结构体，用于记录系统内存状态
 */
struct MemorySnapshot {
    size_t internal_ram;     // 内部RAM可用空间
    size_t total_heap;       // 总堆内存可用空间
    size_t min_heap;         // 最小剩余堆内存
    size_t psram;            // 外部PSRAM可用空间
    size_t dma_capable;      // DMA可用内存
    size_t exec_capable;     // 可执行内存
    
    // 获取内存使用率百分比
    float GetInternalRamUsage(size_t total_internal = 512 * 1024) const {
        return 100.0f * (1.0f - (float)internal_ram / total_internal);
    }
    
    // 获取PSRAM使用率百分比
    float GetPsramUsage(size_t total_psram = 8 * 1024 * 1024) const {
        return 100.0f * (1.0f - (float)psram / total_psram);
    }
    
    // 获取简短的内存状态描述
    std::string GetBriefDescription() const {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), 
                "内部RAM: %.1fKB, 总堆: %.1fKB, PSRAM: %.1fKB", 
                internal_ram / 1024.0f, 
                total_heap / 1024.0f, 
                psram / 1024.0f);
        return std::string(buffer);
    }
    
    // 计算与另一个快照的内存差异
    MemorySnapshot GetDifference(const MemorySnapshot& other) const {
        return {
            other.internal_ram > internal_ram ? 0 : internal_ram - other.internal_ram,
            other.total_heap > total_heap ? 0 : total_heap - other.total_heap,
            other.min_heap > min_heap ? 0 : min_heap - other.min_heap,
            other.psram > psram ? 0 : psram - other.psram,
            other.dma_capable > dma_capable ? 0 : dma_capable - other.dma_capable,
            other.exec_capable > exec_capable ? 0 : exec_capable - other.exec_capable
        };
    }
};

// 函数声明
MemorySnapshot get_memory_snapshot();
void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot, int log_level = 1);
bool check_memory_health(const char* tag, const char* stage, 
                         size_t internal_threshold = 60000, 
                         size_t total_threshold = 100000);

#endif // BLE_TASK_STATE_H