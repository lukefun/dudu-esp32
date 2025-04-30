#ifndef BLE_TASK_STATE_H
#define BLE_TASK_STATE_H

// BLE任务状态枚举
typedef enum {
    BLE_TASK_INIT = 0,      // 任务初始化状态
    BLE_TASK_RUNNING = 1,   // 任务运行中状态
    BLE_TASK_STOPPING = 2,  // 任务正在停止状态
    BLE_TASK_STOPPED = 3    // 任务已停止状态
} ble_task_state_t;

#endif // BLE_TASK_STATE_H