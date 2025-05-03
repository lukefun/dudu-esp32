#include "watchdog_monitor.h"

// 静态成员初始化
int64_t WatchdogMonitor::last_feed_ = 0;
uint32_t WatchdogMonitor::timeout_ = 0;  // 初始化为0，表示未初始化状态 