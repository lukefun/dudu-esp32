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
void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot, int log_level = 3);
bool check_memory_health(const char* tag, const char* stage, 
                         size_t internal_threshold = 60000, 
                         size_t total_threshold = 100000);

#endif // BLE_TASK_STATE_H









// #ifndef BLE_TASK_STATE_H
// #define BLE_TASK_STATE_H

// #include <sys/time.h>
// #include <string>
// #include <esp_heap_caps.h>  // 添加这个头文件以支持heap_caps_*函数
// #include <esp_system.h>     // 添加这个头文件以支持esp_get_minimum_free_heap_size
// #include <esp_log.h>        // 添加这个头文件以支持ESP_LOG*宏

// // BLE任务状态枚举
// typedef enum {
//     BLE_TASK_INIT = 0,      // 任务初始化状态
//     BLE_TASK_RUNNING = 1,   // 任务运行中状态
//     BLE_TASK_STOPPING = 2,  // 任务正在停止状态
//     BLE_TASK_STOPPED = 3    // 任务已停止状态
// } ble_task_state_t;

// // 只保留声明
// std::string GetTimeString();



// // 在文件开头添加内存快照结构体和相关函数
// /*
// 1. internal_ram (MALLOC_CAP_INTERNAL)
// 定义：内部随机存取存储器（RAM）的可用容量。
// 作用：监控ESP32芯片内置RAM的可用空间，内部RAM通常速度更快，用于关键性能操作，对于某些需要高速访问的数据结构和算法至关重要。
// 应用场景：实时处理任务、中断处理程序、需要低延迟的操作。

// 2. total_heap (MALLOC_CAP_8BIT)
// 定义：系统中可用的总堆内存空间。
// 作用：表示整个系统可分配的8位字节对齐内存，包括内部RAM和外部RAM(如果有PSRAM)，是最常用的内存指标，反映系统整体内存健康状况。
// 应用场景：一般内存分配监控、应用程序主要工作内存、判断系统是否有足够资源运行特定功能。

// 3. min_heap (esp_get_minimum_free_heap_size)
// 定义：自系统启动以来观察到的最小剩余堆内存量。
// 作用：反映系统运行过程中遇到的最严重内存压力，帮助识别内存泄漏和峰值内存使用情况，作为系统稳定性的关键指标。
// 应用场景：内存泄漏检测、系统稳定性评估、长期运行系统的健康监控。
// */
// // struct MemorySnapshot {
// //     size_t internal_ram;  // MALLOC_CAP_INTERNAL                内部随机存取存储器（RAM）的容量
// //     size_t total_heap;    // MALLOC_CAP_8BIT                    系统中的总堆内存空间
// //     size_t min_heap;      // esp_get_minimum_free_heap_size()   最小剩余堆内存
// // };

// // MemorySnapshot get_memory_snapshot();
// // void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot);

// /**
//  * @brief 内存快照结构体，用于记录系统内存状态
//  * 
//  * 该结构体包含ESP32系统中各种内存区域的使用情况，
//  * 可用于监控内存使用、诊断内存泄漏和优化内存分配。
//  */
// struct MemorySnapshot {
//     size_t internal_ram;     // 内部RAM可用空间 (MALLOC_CAP_INTERNAL)
//     size_t total_heap;       // 总堆内存可用空间 (MALLOC_CAP_8BIT)
//     size_t min_heap;         // 最小剩余堆内存 (esp_get_minimum_free_heap_size)
//     size_t psram;            // 外部PSRAM可用空间 (MALLOC_CAP_SPIRAM)
//     size_t dma_capable;      // DMA可用内存 (MALLOC_CAP_DMA)
//     size_t exec_capable;     // 可执行内存 (MALLOC_CAP_EXEC)
    
//     // 获取内存使用率百分比
//     float GetInternalRamUsage(size_t total_internal = 512 * 1024) const {
//         return 100.0f * (1.0f - (float)internal_ram / total_internal);
//     }
    
//     // 获取PSRAM使用率百分比
//     float GetPsramUsage(size_t total_psram = 8 * 1024 * 1024) const {
//         return 100.0f * (1.0f - (float)psram / total_psram);
//     }
    
//     // 获取简短的内存状态描述
//     std::string GetBriefDescription() const {
//         char buffer[128];
//         snprintf(buffer, sizeof(buffer), 
//                 "内部RAM: %.1fKB, 总堆: %.1fKB, PSRAM: %.1fKB", 
//                 internal_ram / 1024.0f, 
//                 total_heap / 1024.0f, 
//                 psram / 1024.0f);
//         return std::string(buffer);
//     }
    
//     // 计算与另一个快照的内存差异
//     MemorySnapshot GetDifference(const MemorySnapshot& other) const {
//         return {
//             other.internal_ram > internal_ram ? 0 : internal_ram - other.internal_ram,
//             other.total_heap > total_heap ? 0 : total_heap - other.total_heap,
//             other.min_heap > min_heap ? 0 : min_heap - other.min_heap,
//             other.psram > psram ? 0 : psram - other.psram,
//             other.dma_capable > dma_capable ? 0 : dma_capable - other.dma_capable,
//             other.exec_capable > exec_capable ? 0 : exec_capable - other.exec_capable
//         };
//     }
// };

// /**
//  * @brief 获取当前系统内存快照
//  * 
//  * @return MemorySnapshot 包含当前系统各内存区域使用情况的快照
//  */
// MemorySnapshot get_memory_snapshot();

// /**
//  * @brief 记录内存状态日志
//  * 
//  * @param tag 日志标签
//  * @param stage 当前阶段描述
//  * @param snapshot 内存快照
//  * @param log_level 日志级别 (0=错误, 1=警告, 2=信息, 3=调试, 4=详细)
//  */
// void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot, int log_level = 3);

// /**
//  * @brief 检查内存状态并在低于阈值时发出警告
//  * 
//  * @param tag 日志标签
//  * @param stage 当前阶段描述
//  * @param internal_threshold 内部RAM警告阈值（字节）
//  * @param total_threshold 总堆内存警告阈值（字节）
//  * @return true 如果内存状态正常
//  * @return false 如果内存低于阈值
//  */
// bool check_memory_health(const char* tag, const char* stage, 
//                          size_t internal_threshold = 60000, 
//                          size_t total_threshold = 100000);

// #endif // BLE_TASK_STATE_H