#include "ble_config.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"  // 添加任务看门狗头文件
#include "freertos/FreeRTOS.h"  // FreeRTOS头文件
#include "freertos/timers.h"    // FreeRTOS定时器头文件
#include "../system_info.h"  // 引入SystemInfo类定义
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>

static const char* TAG = "BLE_CONFIG";

// 定义静态成员变量
TaskHandle_t BleConfig::ble_host_task_handle = nullptr;
volatile bool BleConfig::ble_host_task_running = true;
volatile ble_task_state_t BleConfig::ble_host_task_state = BLE_TASK_INIT;

// 获取当前时间字符串，包含毫秒
static std::string GetTimeString() {
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


// 在文件开头添加内存快照结构体和相关函数
/*
1. internal_ram (MALLOC_CAP_INTERNAL)
定义：内部随机存取存储器（RAM）的可用容量。
作用：监控ESP32芯片内置RAM的可用空间，内部RAM通常速度更快，用于关键性能操作，对于某些需要高速访问的数据结构和算法至关重要。
应用场景：实时处理任务、中断处理程序、需要低延迟的操作。

2. total_heap (MALLOC_CAP_8BIT)
定义：系统中可用的总堆内存空间。
作用：表示整个系统可分配的8位字节对齐内存，包括内部RAM和外部RAM(如果有PSRAM)，是最常用的内存指标，反映系统整体内存健康状况。
应用场景：一般内存分配监控、应用程序主要工作内存、判断系统是否有足够资源运行特定功能。

3. min_heap (esp_get_minimum_free_heap_size)
定义：自系统启动以来观察到的最小剩余堆内存量。
作用：反映系统运行过程中遇到的最严重内存压力，帮助识别内存泄漏和峰值内存使用情况，作为系统稳定性的关键指标。
应用场景：内存泄漏检测、系统稳定性评估、长期运行系统的健康监控。
*/
struct MemorySnapshot {
    size_t internal_ram;  // MALLOC_CAP_INTERNAL                内部随机存取存储器（RAM）的容量
    size_t total_heap;    // MALLOC_CAP_8BIT                    系统中的总堆内存空间
    size_t min_heap;      // esp_get_minimum_free_heap_size()   最小剩余堆内存
};

// 获取当前内存状态
static MemorySnapshot get_memory_snapshot() {
    return {
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_8BIT),
        esp_get_minimum_free_heap_size()
    };
}

// 打印内存状态日志
static void log_memory_state(const char* tag, const char* stage, const MemorySnapshot& snapshot) {
    ESP_LOGI(tag, "%s @%s: 内存状态 - 内部RAM: %zu字节, 总堆内存: %zu字节, 最小剩余堆内存: %zu字节", 
             GetTimeString().c_str(), stage, 
             snapshot.internal_ram, snapshot.total_heap, snapshot.min_heap);
}

static BleConfig* g_ble_config_instance = nullptr;
extern "C" void ble_store_config_init(void);

static ble_uuid128_t gatt_svr_svc_wifi_config_uuid;
static ble_uuid128_t gatt_svr_chr_ssid_uuid;
static ble_uuid128_t gatt_svr_chr_password_uuid;
static ble_uuid128_t gatt_svr_chr_control_status_uuid;

// 新增常量定义
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define CONTROL_CMD_LEN 1

// BLE广播间隔常量 (单位: 0.625ms)
#define BLE_GAP_ADV_FAST_INTERVAL_MIN1 0x0030 // 30ms  
#define BLE_GAP_ADV_FAST_INTERVAL_MAX1 0x0050 // 50ms

// 通知重试配置
#define NOTIFY_RETRY_COUNT 3      // 最大重试次数
#define NOTIFY_RETRY_DELAY_MS 100 // 重试间隔(ms)

static char* bytes_to_hex(const uint8_t* bytes, size_t len) {
    static char hex_str[512];
    for(size_t i=0; i<len && i<sizeof(hex_str)/2-1; i++) {
        sprintf(hex_str+i*2, "%02x", bytes[i]);
    }
    return hex_str;
}

static void parse_all_uuids() {
    int rc;
    ESP_LOGI(TAG, "%s @parse_all_uuids: 开始解析UUID...", GetTimeString().c_str());

    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_svc_wifi_config_uuid.u, WIFI_CONFIG_SERVICE_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_ssid_uuid.u, SSID_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_password_uuid.u, PASSWORD_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_control_status_uuid.u, CONTROL_STATUS_CHAR_UUID);
    assert(rc == 0);
    
    ESP_LOGI(TAG, "%s @parse_all_uuids: UUID解析完成", GetTimeString().c_str());
}

static const struct ble_gatt_chr_def gatt_svr_characteristics[] = {
    // SSID特征值
    {
        .uuid = &gatt_svr_chr_ssid_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"ssid",
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE,
        .min_key_size = 16,
        .val_handle = NULL,
        .cpfd = NULL
    },
    // Password特征值
    {
        .uuid = &gatt_svr_chr_password_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"password",
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE,
        .min_key_size = 16,
        .val_handle = NULL,
        .cpfd = NULL
    },
    // Control特征值
    {
        .uuid = &gatt_svr_chr_control_status_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"control",
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 16,
        .val_handle = NULL,
        .cpfd = NULL
    },
    { 0 } // 结束标记
};

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_wifi_config_uuid.u,
        .characteristics = gatt_svr_characteristics
    },
    { 0 }
};

void BleConfig::gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG, "%s @gatt_svr_register_cb: registered service %s with handle=0x%04x",
               GetTimeString().c_str(), ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
               ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG, "%s @gatt_svr_register_cb: registering characteristic %s with def_handle=0x%04x val_handle=0x%04x",
               GetTimeString().c_str(), ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
               ctxt->chr.def_handle,
               ctxt->chr.val_handle);
               
        if (g_ble_config_instance && 
            ble_uuid_cmp(ctxt->chr.chr_def->uuid, &gatt_svr_chr_control_status_uuid.u) == 0) {
            g_ble_config_instance->status_val_handle_ = ctxt->chr.val_handle;
            ESP_LOGI(TAG, "%s @gatt_svr_register_cb: 保存控制状态特征值句柄: 0x%04x", GetTimeString().c_str(), ctxt->chr.val_handle);
        }
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGI(TAG, "%s @gatt_svr_register_cb: registering descriptor %s with handle=0x%04x",
               GetTimeString().c_str(), ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
               ctxt->dsc.handle);
        break;
    default:
        assert(0);
        break;
    }
}

void BleConfig::Initialize() {
    // 初始化时记录初始内存状态
    MemorySnapshot initial_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "Initialize: 初始内存状态", initial_snapshot);

    g_ble_config_instance = this;
    ESP_LOGI(TAG, "%s @Initialize: 开始初始化BLE配网模块...", GetTimeString().c_str());

    // 重置看门狗
    esp_task_wdt_reset();

    // 完全避免尝试清理资源的过程，直接初始化
    ESP_LOGI(TAG, "%s @Initialize: 跳过清理步骤，直接初始化NimBLE", GetTimeString().c_str());

    // 1. 初始化NVS
    ESP_LOGI(TAG, "%s @Initialize: 步骤1 - 初始化NVS存储...", GetTimeString().c_str());
    log_memory_state(TAG, "NVS初始化前", get_memory_snapshot());
    esp_task_wdt_reset();
    
    esp_err_t nvs_ret = nvs_flash_init();
    esp_task_wdt_reset();
    
    log_memory_state(TAG, "NVS初始化后", get_memory_snapshot());

    // 如果NVS分区已满或版本不匹配，则擦除重新初始化
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s @Initialize: NVS需要擦除", GetTimeString().c_str());
        esp_task_wdt_reset();
        nvs_flash_erase();
        esp_task_wdt_reset();
        nvs_ret = nvs_flash_init();
        esp_task_wdt_reset();
    }

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "%s @Initialize: NVS初始化失败: %s", GetTimeString().c_str(), esp_err_to_name(nvs_ret));
        return;
    }
    ESP_LOGI(TAG, "%s @Initialize: NVS初始化成功", GetTimeString().c_str());
    esp_task_wdt_reset();

    // 2. 解析服务和特征UUID
    ESP_LOGI(TAG, "%s @Initialize: 步骤2 - 解析BLE服务UUID", GetTimeString().c_str());
    log_memory_state(TAG, "UUID解析前", get_memory_snapshot());
    esp_task_wdt_reset();
    
    parse_all_uuids();
    
    esp_task_wdt_reset();
    log_memory_state(TAG, "UUID解析后", get_memory_snapshot());

    // 3. 检查内存是否足够
    MemorySnapshot before_ble = get_memory_snapshot();
    ESP_LOGI(TAG, "%s @Initialize: BLE初始化前可用堆内存: %zu 字节", GetTimeString().c_str(), before_ble.total_heap);
    esp_task_wdt_reset();
    
    if (before_ble.total_heap < 60000) {
        ESP_LOGW(TAG, "%s @Initialize: 可用内存较低，但仍将继续初始化", GetTimeString().c_str());
        esp_task_wdt_reset();
        heap_caps_check_integrity_all(true);
        esp_task_wdt_reset();
    }

    // 4. 初始化NimBLE
    ESP_LOGI(TAG, "%s @Initialize: 步骤4 - 初始化NimBLE端口", GetTimeString().c_str());
    log_memory_state(TAG, "nimble_port_init()前", get_memory_snapshot());
    esp_task_wdt_reset();   // 重置看门狗，NimBLE初始化前
    
    esp_err_t rc = nimble_port_init();
    
    esp_task_wdt_reset();   // 重置看门狗，NimBLE初始化后
    log_memory_state(TAG, "nimble_port_init()后", get_memory_snapshot());
    
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "%s @Initialize: NimBLE端口初始化失败: %d", GetTimeString().c_str(), rc);
        return;
    }
    ESP_LOGI(TAG, "%s @Initialize: NimBLE端口初始化成功", GetTimeString().c_str());

    // 5. 配置BLE安全参数
    ESP_LOGI(TAG, "%s @Initialize: 配置BLE安全参数", GetTimeString().c_str());
    esp_task_wdt_reset(); // 重置看门狗，安全参数配置前
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    // 6. 设置回调函数
    ESP_LOGI(TAG, "%s @Initialize: 设置BLE回调函数", GetTimeString().c_str());
    esp_task_wdt_reset(); // 重置看门狗，回调设置前
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = NULL;

    // 7. 初始化GATT服务器
    ESP_LOGI(TAG, "%s @Initialize: 步骤7 - 初始化GATT服务器", GetTimeString().c_str());
    log_memory_state(TAG, "gatt_svr_init()前", get_memory_snapshot());
    esp_task_wdt_reset();   // 重置看门狗，GATT初始化前
    
    gatt_svr_init();        // GATT初始化
    
    esp_task_wdt_reset();   // 重置看门狗，GATT初始化后
    log_memory_state(TAG, "gatt_svr_init()后", get_memory_snapshot());

    // 8. 设置设备名称 - 格式：DUDU-BLE-[MAC后6位]
    ESP_LOGI(TAG, "%s @Initialize: 设置BLE设备名称", GetTimeString().c_str());
    esp_task_wdt_reset(); // 重置看门狗，设置设备名称前

    // 获取MAC地址
    std::string mac_address = SystemInfo::GetMacAddress();
    // 提取MAC地址后6位（去除冒号）
    std::string mac_suffix;
    for (size_t i = mac_address.length() - 8; i < mac_address.length(); i++) {
        if (mac_address[i] != ':') {
            mac_suffix += mac_address[i];
        }
    }
    // 组合设备名称
    std::string device_name = std::string(kBleDeviceName) + "-" + mac_suffix;
    ESP_LOGI(TAG, "%s @Initialize: 设备名称: %s", GetTimeString().c_str(), device_name.c_str());

    // 设置设备名称
    log_memory_state(TAG, "ble_svc_gap_device_name_set()前", get_memory_snapshot());
    int name_rc = ble_svc_gap_device_name_set(device_name.c_str());
    log_memory_state(TAG, "ble_svc_gap_device_name_set()后", get_memory_snapshot());
    
    if (name_rc != 0) {
        ESP_LOGW(TAG, "%s @Initialize: 设置设备名称失败: %d", GetTimeString().c_str(), name_rc);
    }

    // 9. 初始化BLE存储
    ESP_LOGI(TAG, "%s @Initialize: 步骤9 - 初始化BLE存储", GetTimeString().c_str());
    log_memory_state(TAG, "ble_store_config_init()前", get_memory_snapshot());
    esp_task_wdt_reset();
    
    ble_store_config_init();
    
    esp_task_wdt_reset();
    log_memory_state(TAG, "ble_store_config_init()后", get_memory_snapshot());

    // 任务创建前后对比内存
    MemorySnapshot pre_task = get_memory_snapshot();
    log_memory_state(TAG, "ble_host_task创建前", pre_task);

    // 10. 启动BLE主机任务
    ESP_LOGI(TAG, "%s @Initialize: 创建BLE主机任务", GetTimeString().c_str());
    esp_task_wdt_reset();
    xTaskCreatePinnedToCore(
        ble_host_task, 
        "ble_host_task",
        16384,        // 16KB栈大小
        NULL,         // 不传递参数
        10,           // 改回 10
        &ble_host_task_handle, // <<< 保存任务句柄到成员变量
        0             // 在核心0上运行
    );

    MemorySnapshot post_task = get_memory_snapshot();
    log_memory_state(TAG, "ble_host_task创建后", post_task);
    ESP_LOGI(TAG, "%s @Initialize: 任务大致占用: %d字节", GetTimeString().c_str(), 
             (int)(pre_task.total_heap - post_task.total_heap));

    // 等待一小段时间，让BLE任务启动
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_task_wdt_reset();
    
    // 最终内存状态
    MemorySnapshot final_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "Initialize完成时最终内存状态", final_snapshot);
    ESP_LOGI(TAG, "%s @Initialize: 初始化总占用: %d字节", GetTimeString().c_str(), 
             (int)(initial_snapshot.total_heap - final_snapshot.total_heap));
    
    ESP_LOGI(TAG, "%s @Initialize: BLE初始化完成", GetTimeString().c_str());
    esp_task_wdt_reset();
}

// 新增的函数，用于处理GATT特征值的访问，包括SSID、Password和Control，并更新状态（GATT特征值：）
// GATT特征值：
// 1. SSID特征值：用于存储WiFi的SSID
// 2. Password特征值：用于存储WiFi的Password
// 3. Control特征值：用于控制WiFi的连接状态
int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char* char_name = (const char*)arg;
    int rc = 0;

    // 确保全局实例存在
    if (!g_ble_config_instance) {
        ESP_LOGE(TAG, "%s @gatt_svr_chr_access: GATT访问失败: 全局实例不存在", GetTimeString().c_str());
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 根据特征值名称进行处理
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {    // 写入特征值
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);    // 获取数据长度
            ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 收到特征值写入请求: %s, 数据长度: %d", GetTimeString().c_str(), char_name, data_len);

            // ==== 新增数据校验开始 ====
            // 【控制命令】必须为1字节，且必须为0x01
            if(strcmp(char_name, "control") == 0 && data_len != CONTROL_CMD_LEN) {
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: 控制命令长度无效: %d, 应为1字节", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // 【SSID】最大32字节
            if(strcmp(char_name, "ssid") == 0 && data_len > MAX_SSID_LEN) {
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: SSID长度过长: %d/32", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; 
            }

            // 【密码】最大64字节
            if(strcmp(char_name, "password") == 0 && data_len > MAX_PASSWORD_LEN) {
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: 密码长度过长: %d/64", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // ==== 新增数据校验结束 ====

            if (data_len > 256) {   // 原有长度检查
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: 数据长度超过最大限制: %d/256", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            uint8_t* data = (uint8_t*)malloc(data_len + 1);
            if (!data) {
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: 内存分配失败，无法处理接收数据", GetTimeString().c_str());
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            
            // 转换数据格式，并添加结束符
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "%s @gatt_svr_chr_access: 数据转换失败: %d", GetTimeString().c_str(), rc);
                free(data);
                return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0';

            ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 接收到 %d 字节数据: %.*s (十六进制: %s)", 
                    GetTimeString().c_str(), data_len, data_len, data, bytes_to_hex(data, data_len));

            // 处理接收到的数据
            if (char_name) {    // 确保char_name不为空
                if (strcmp(char_name, "ssid") == 0) {
                    // 保存SSID 到成员变量，将接收到的 WiFi SSID 数据（data 指针，长度 data_len）赋值给 BleConfig 实例的成员变量 received_ssid_ 。
                    g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 保存SSID: %s", GetTimeString().c_str(), g_ble_config_instance->received_ssid_.c_str());
                } else if (strcmp(char_name, "password") == 0) {
                    // 保存密码 到成员变量，将接收到的 WiFi 密码 数据（data 指针，长度 data_len）赋值给 BleConfig 实例的成员变量 received_password_ 。
                    g_ble_config_instance->received_password_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 保存密码: %s", GetTimeString().c_str(), g_ble_config_instance->received_password_.c_str());
                } else if (strcmp(char_name, "control") == 0 && 
                          data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {    // 连接WiFi命令，且数据长度为1字节，且数据为0x01，则连接WiFi，并清空SSID和密码
                    ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 收到连接WiFi命令", GetTimeString().c_str());
                    if (!g_ble_config_instance->received_ssid_.empty() && 
                        !g_ble_config_instance->received_password_.empty()) {
                        // 调用凭据接收回调，并清空SSID和密码
                        ESP_LOGI(TAG, "%s @gatt_svr_chr_access: SSID和密码已接收，准备连接WiFi", GetTimeString().c_str());
                        if (g_ble_config_instance->credentials_received_cb_) {
                            ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 调用凭据接收回调", GetTimeString().c_str());

                            g_ble_config_instance->credentials_received_cb_(    // 调用凭据接收回调
                                g_ble_config_instance->received_ssid_,          // SSID
                                g_ble_config_instance->received_password_);     // Password
                        }

                        if (g_ble_config_instance->connect_wifi_cb_) {          // 调用WiFi连接回调
                            ESP_LOGI(TAG, "%s @gatt_svr_chr_access: 调用WiFi连接回调", GetTimeString().c_str());
                            g_ble_config_instance->connect_wifi_cb_();
                        }
                    } else {
                        ESP_LOGW(TAG, "%s @gatt_svr_chr_access: 收到连接命令但SSID或密码为空", GetTimeString().c_str());
                    }
                }
            }
            // 释放内存
            free(data);
            break;
        }
        default:
            ESP_LOGW(TAG, "%s @gatt_svr_chr_access: 不支持的GATT操作: %d", GetTimeString().c_str(), ctxt->op);
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return rc;
}

void BleConfig::gatt_svr_init(void) {
    ESP_LOGI(TAG, "%s @gatt_svr_init: 初始化GATT服务器...", GetTimeString().c_str());
    int rc;
    
    // 配置GATT服务
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s @gatt_svr_init: GATT服务计数配置失败: %d", GetTimeString().c_str(), rc);
        return; // 失败时直接返回，不使用assert
    }
    
    // 添加GATT服务
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s @gatt_svr_init: 添加GATT服务失败: %d", GetTimeString().c_str(), rc);
        return; // 失败时直接返回，不使用assert
    }
    
    ESP_LOGI(TAG, "%s @gatt_svr_init: GATT服务器初始化成功", GetTimeString().c_str());
}

void BleConfig::ble_advertise(void) {
    ESP_LOGI(TAG, "%s @ble_advertise: 准备开始BLE广播...", GetTimeString().c_str());
    const int MAX_RETRY = 3;  // 最大重试次数
    int retry_count = 0;
    
    // 先检查是否已有广播在运行，如果有则先停止
    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "%s @ble_advertise: 检测到广播已在运行，先停止当前广播", GetTimeString().c_str());
        int rc = ble_gap_adv_stop();
        if (rc != 0) {
            ESP_LOGW(TAG, "%s @ble_advertise: 停止当前广播失败: %d，继续尝试启动新广播", GetTimeString().c_str(), rc);
        } else {
            ESP_LOGI(TAG, "%s @ble_advertise: 已停止当前广播，准备启动新广播", GetTimeString().c_str());
        }
        // 添加短暂延时，确保BLE栈有足够时间处理停止操作
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    do {
        struct ble_gap_adv_params adv_params;   // 广播参数
        struct ble_hs_adv_fields fields;        // 广播字段
        struct ble_hs_adv_fields rsp_fields;    // 扫描响应字段
        
        memset(&fields, 0, sizeof fields);      // 清空广播字段
        memset(&rsp_fields, 0, sizeof rsp_fields); // 清空扫描响应字段
        
        // 使用最简单的广播字段配置
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;    // 广播类型
        
        // 设置设备名称
        const char* dev_name = ble_svc_gap_device_name();
        fields.name = (uint8_t *)dev_name;                                 // 广播名称
        fields.name_len = strlen(dev_name);                                // 广播名称长度
        fields.name_is_complete = 1;                                       // 使用完整名称

        // 将服务UUID放入扫描响应包，便于小程序过滤
        rsp_fields.num_uuids128 = 1;
        rsp_fields.uuids128 = &gatt_svr_svc_wifi_config_uuid;

        ESP_LOGI(TAG, "%s @ble_advertise: 设置广播字段，设备名称: %s", GetTimeString().c_str(), dev_name);

        int rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "%s @ble_advertise: 设置广播字段失败: %d，重试次数: %d/%d", GetTimeString().c_str(), rc, retry_count + 1, MAX_RETRY);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // 设置扫描响应字段
        rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "%s @ble_advertise: 设置扫描响应字段失败: %d，重试次数: %d/%d", GetTimeString().c_str(), rc, retry_count + 1, MAX_RETRY);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 设置标准广播参数
        memset(&adv_params, 0, sizeof adv_params);                  // 清空广播参数
        adv_params.itvl_min = 32;  // 20ms (32 * 0.625ms)
        adv_params.itvl_max = 48;  // 30ms (48 * 0.625ms)
        adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;               // 连接模式
        adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;               // 发现模式

        ESP_LOGI(TAG, "%s @ble_advertise: 开始广播，间隔: %d-%d (单位: 0.625ms)", GetTimeString().c_str(), adv_params.itvl_min, adv_params.itvl_max);

        // 开始广播
        rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, ble_gap_event, NULL);

        if (rc == 0) {
            ESP_LOGI(TAG, "%s @ble_advertise: BLE广播已成功启动", GetTimeString().c_str());
            return;  // 广播成功，退出函数
        } else if (rc == BLE_HS_EALREADY) {
            // 错误码2表示已经有广播在运行
            ESP_LOGW(TAG, "%s @ble_advertise: 广播已在运行(BLE_HS_EALREADY)，尝试停止后重新启动", GetTimeString().c_str());
            ble_gap_adv_stop();
            vTaskDelay(pdMS_TO_TICKS(200));  // 给BLE栈一些时间处理停止操作
        } else {
            ESP_LOGE(TAG, "%s @ble_advertise: 启动BLE广播失败: %d，重试次数: %d/%d", GetTimeString().c_str(), rc, retry_count + 1, MAX_RETRY);
        }
        
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));  // 延迟1秒后重试
        
    } while (retry_count < MAX_RETRY);
    
    ESP_LOGE(TAG, "%s @ble_advertise: BLE广播启动失败，已达到最大重试次数", GetTimeString().c_str());
}

bool BleConfig::StartAdvertising() {
    ESP_LOGI(TAG, "%s @StartAdvertising: 尝试开始BLE广播...", GetTimeString().c_str());
    
    // 检查BLE主机是否已同步
    if (ble_hs_synced()) {
        ESP_LOGI(TAG, "%s @StartAdvertising: BLE主机已同步，准备开始广播", GetTimeString().c_str());
        
        // 检查是否已有广播在运行
        if (ble_gap_adv_active()) {
            ESP_LOGI(TAG, "%s @StartAdvertising: 检测到广播已在运行，先停止当前广播", GetTimeString().c_str());
            int rc = ble_gap_adv_stop();
            if (rc != 0) {
                ESP_LOGW(TAG, "%s @StartAdvertising: 停止当前广播失败: %d，但仍将尝试启动新广播", GetTimeString().c_str(), rc);
            } else {
                ESP_LOGI(TAG, "%s @StartAdvertising: 已停止当前广播", GetTimeString().c_str());
            }
            // 添加短暂延时，确保BLE栈有足够时间处理停止操作
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 启动广播
        ble_advertise();
        return true; // 广播启动成功
    } else {
        ESP_LOGW(TAG, "%s @StartAdvertising: BLE主机尚未同步，将在同步后自动开始广播", GetTimeString().c_str());
        return false; // 当前无法启动广播，需等待同步
    }
}

void BleConfig::StopAdvertising() {
    ESP_LOGI(TAG, "%s @StopAdvertising: 尝试停止BLE广播...", GetTimeString().c_str());
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            ESP_LOGI(TAG, "%s @StopAdvertising: BLE广播已成功停止", GetTimeString().c_str());
        } else {
            ESP_LOGE(TAG, "%s @StopAdvertising: 停止BLE广播失败: %d", GetTimeString().c_str(), rc);
        }
    } else {
        ESP_LOGI(TAG, "%s @StopAdvertising: BLE广播已经处于停止状态", GetTimeString().c_str());
    }
}

void BleConfig::ble_on_sync(void) {
    ESP_LOGI(TAG, "%s @ble_on_sync: BLE主机同步完成，准备开始广播", GetTimeString().c_str());

    // 获取设备地址
    uint8_t addr_val[6] = {0};
    int rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "%s @ble_on_sync: 设备MAC地址: %02x:%02x:%02x:%02x:%02x:%02x",
                 GetTimeString().c_str(), addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);
    } else {
        ESP_LOGE(TAG, "%s @ble_on_sync: 获取设备地址失败: %d", GetTimeString().c_str(), rc);
    }

    // 开始广播
    if (g_ble_config_instance) {
        ESP_LOGI(TAG, "%s @ble_on_sync: BLE实例已初始化，开始广播", GetTimeString().c_str());
        g_ble_config_instance->ble_advertise();
    } else {
        ESP_LOGE(TAG, "%s @ble_on_sync: BLE实例未初始化，无法开始广播", GetTimeString().c_str());
    }
}

void BleConfig::ble_on_reset(int reason) {
    ESP_LOGE(TAG, "%s @ble_on_reset: BLE主机重置，原因: %d", GetTimeString().c_str(), reason);
}

// BLE主机任务，负责管理BLE主机的生命周期，包括初始化、同步、广播等
void BleConfig::ble_host_task(void *param) {
    // 记录任务启动时的内存状态
    // size_t start_free_internal_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // size_t start_free_total_mem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    // size_t start_min_free_heap = esp_get_minimum_free_heap_size();
    // ESP_LOGI(TAG, "%s @ble_host_task: BLE主机任务启动【特别关注！】内部RAM: %zu字节, 总堆内存: %zu字节, 最小剩余堆内存: %zu字节", 
    //          GetTimeString().c_str(), start_free_internal_mem, start_free_total_mem, start_min_free_heap);

    MemorySnapshot start_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "ble_host_task: BLE主机任务启动", start_snapshot);


    BleConfig::ble_host_task_state = BLE_TASK_RUNNING;
    BleConfig::ble_host_task_running = true;
    ESP_LOGI(TAG, "%s @ble_host_task: BLE主机任务状态设置为RUNNING", GetTimeString().c_str());
    
    // 循环计数器
    int loop_counter = 0;
    
    // BLE主循环
    while (BleConfig::ble_host_task_running) {
        // 每10次循环记录一次调试日志
        if (++loop_counter % 10 == 0) {
            ESP_LOGD(TAG, "%s @ble_host_task: BLE主循环仍在运行，迭代次数: %d", GetTimeString().c_str(), loop_counter);
        }

        // 记录进入nimble_port_run前的时间戳
        int64_t start_time = esp_timer_get_time();
        ESP_LOGD(TAG, "%s @ble_host_task: 正在进入nimble_port_run()阻塞调用，时间戳: %lld", 
                 GetTimeString().c_str(), start_time);

        nimble_port_run(); // NimBLE事件循环（这是一个会阻塞并处理事件的函数）
                           // 它内部应该有让出CPU的机制，或者其事件处理不应耗时过长
                           // 如果nimble_port_run()本身卡死，此处的喂狗也无法执行
        
        // 记录从nimble_port_run返回的时间和阻塞时长
        int64_t end_time = esp_timer_get_time();
        int64_t blocked_time_us = end_time - start_time;
        ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_run() 执行耗时: %lld us", GetTimeString().c_str(), blocked_time_us);

        // 在此处添加适当的延时，避免CPU占用过高
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 主动释放NimBLE资源
    ESP_LOGI(TAG, "%s @ble_host_task: 收到退出信号，开始资源清理", GetTimeString().c_str());
    
    // 记录释放前的内存状态
    MemorySnapshot before_deinit = get_memory_snapshot();
    log_memory_state(TAG, "ble_host_task: nimble_port_deinit()前", before_deinit);

    // size_t before_deinit_free_internal_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // size_t before_deinit_free_total_mem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    // size_t before_deinit_min_free_heap = esp_get_minimum_free_heap_size();
    // ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_deinit()前内存状态 - 内部RAM: %zu字节, 总堆内存: %zu字节, 最小剩余堆内存: %zu字节", 
    //          GetTimeString().c_str(), before_deinit_free_internal_mem, before_deinit_free_total_mem, before_deinit_min_free_heap);
    

    ESP_LOGI(TAG, "%s @ble_host_task: 调用 nimble_port_stop()", GetTimeString().c_str());
    nimble_port_stop(); // 停止NimBLE端口事件处理
    ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_stop() 调用完成", GetTimeString().c_str());

    // 在 nimble_port_deinit() 之前调用 ble_hs_deinit()
    ESP_LOGI(TAG, "%s @ble_host_task: 调用 ble_hs_deinit()", GetTimeString().c_str());
    ble_hs_deinit();
    ESP_LOGI(TAG, "%s @ble_host_task: ble_hs_deinit() 调用完成", GetTimeString().c_str());

    // nimble_port_deinit() 将由 Deinitialize() 函数统一处理
    // ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_deinit();    // 释放NimBLE资源", GetTimeString().c_str());
    ESP_LOGI(TAG, "%s @ble_host_task: BLE主机任务正常退出，资源清理将由Deinitialize处理", GetTimeString().c_str());
    
    // 记录释放后的内存状态
    MemorySnapshot after_deinit = get_memory_snapshot();
    log_memory_state(TAG, "ble_host_task: nimble_port_deinit()后", after_deinit);
    ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_deinit释放: %d字节", 
             GetTimeString().c_str(), (int)(after_deinit.total_heap - before_deinit.total_heap));
    ESP_LOGI(TAG, "%s @ble_host_task: BLE主机任务退出前最终内存状态, 任务生命周期内总消耗(估算): %d字节", 
             GetTimeString().c_str(), (int)(start_snapshot.total_heap - after_deinit.total_heap));




    // size_t after_deinit_free_internal_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // size_t after_deinit_free_total_mem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    // size_t after_deinit_min_free_heap = esp_get_minimum_free_heap_size();
    // ESP_LOGI(TAG, "%s @ble_host_task: nimble_port_deinit()后内存状态 - 内部RAM: %zu字节, 总堆内存: %zu字节, 最小剩余堆内存: %zu字节, nimble_port_deinit释放: %d字节", 
    //          GetTimeString().c_str(), after_deinit_free_internal_mem, after_deinit_free_total_mem, after_deinit_min_free_heap,
    //          (int)(after_deinit_free_total_mem - before_deinit_free_total_mem));
    // ESP_LOGI(TAG, "%s @ble_host_task: BLE主机任务退出前最终内存状态 - 内部RAM: %zu字节, 总堆内存: %zu字节, 最小剩余堆内存: %zu字节, 任务生命周期内总消耗(估算): %d字节", 
    //          GetTimeString().c_str(), after_deinit_free_internal_mem, after_deinit_free_total_mem, after_deinit_min_free_heap,
    //          (int)(start_free_total_mem - after_deinit_free_total_mem));
    
    BleConfig::ble_host_task_state = BLE_TASK_STOPPED;     // 设置任务状态为已停止
    ESP_LOGI(TAG, "%s @ble_host_task: 任务状态已设置为STOPPED，准备自杀退出", GetTimeString().c_str());
    vTaskDelete(NULL); // 任务自杀，安全退出
}


void BleConfig::SendWifiStatus(wifi_config_status_t status) {

    ESP_LOGI(TAG, "%s @SendWifiStatus: 尝试发送WiFi状态: %d", GetTimeString().c_str(), status);
    
    // 检查是否有连接
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || status_val_handle_ == 0) {
        ESP_LOGW(TAG, "%s @SendWifiStatus: 无法发送状态，没有连接或句柄无效 (conn_handle=%d, status_val_handle=%d)", 
                GetTimeString().c_str(), conn_handle_, status_val_handle_);
        return;
    }

    // 分配内存前先检查可用堆内存
    MemorySnapshot pre_alloc = get_memory_snapshot();
    log_memory_state(TAG, "@SendWifiStatus: 分配内存前先检查可用堆内存", pre_alloc);

    // size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    // ESP_LOGI(TAG, "%s @SendWifiStatus: 分配内存前先检查可用堆内存，当前可用堆内存: %d 字节", GetTimeString().c_str(), free_heap);

    esp_task_wdt_reset();   // 重置任务看门狗
    
    // 分配内存
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&status, sizeof(status));
    if (!om) {
        ESP_LOGE(TAG, "%s @SendWifiStatus: 通知：分配内存失败", GetTimeString().c_str());
        return;
    }

    // 增加短暂延时，确保BLE栈有足够时间处理
    int rc = -1;

    // 重试发送通知
    for(int retry = 0; retry < NOTIFY_RETRY_COUNT; retry++) {
        
        // 发送通知，如果成功则退出循环，否则继续重试
        rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
        if(rc == 0) {
            ESP_LOGI(TAG, "%s @SendWifiStatus: WiFi状态通知发送成功: %d", GetTimeString().c_str(), status);
            break;
        }

        ESP_LOGW(TAG, "%s @SendWifiStatus: 通知发送失败 (尝试 %d/%d)，错误码: %d，稍后重试...", 
                GetTimeString().c_str(), retry+1, NOTIFY_RETRY_COUNT, rc);

        // 延时后再次尝试发送通知
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "%s @SendWifiStatus: 发送通知失败，所有重试均失败; rc=%d", GetTimeString().c_str(), rc);
    }

    os_mbuf_free_chain(om);  // 确保释放mbuf
}

void BleConfig::SetCredentialsReceivedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    ESP_LOGI(TAG, "%s @SetCredentialsReceivedCallback: 设置凭据接收回调", GetTimeString().c_str());
    credentials_received_cb_ = cb;
}

void BleConfig::SetConnectWifiCallback(std::function<void()> cb) {
    ESP_LOGI(TAG, "%s @SetConnectWifiCallback: 设置WiFi连接回调", GetTimeString().c_str());
    connect_wifi_cb_ = cb;
}

// 处理 GATT 事件，如连接建立、断开等，并调用相应的回调函数，如凭据接收、WiFi连接等，以实现设备与手机的通信，并在手机端显示连接状态和接收的 Wi-Fi 凭据
int BleConfig::ble_gap_event(struct ble_gap_event *event, void *arg) {
    // 确保实例存在
    struct ble_gap_conn_desc desc;
    int rc;
    if (!g_ble_config_instance) {
        ESP_LOGE(TAG, "%s @ble_gap_event: BLE事件处理失败：全局实例不存在", GetTimeString().c_str());
        return 0;
    }

    ESP_LOGD(TAG, "%s @ble_gap_event: 收到BLE事件: %d", GetTimeString().c_str(), event->type);
    
    switch (event->type) {

    // === 新增加密状态处理 === 
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.status == 0) {
            ESP_LOGI(TAG, "%s @ble_gap_event: 加密状态变更: %s", 
                    GetTimeString().c_str(), event->enc_change.status == 0 ? "已加密" : "未加密");
        } else {
            ESP_LOGE(TAG, "%s @ble_gap_event: 加密失败, 状态码: %d", GetTimeString().c_str(), event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: CONNECT事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: BLE连接事件 - 状态: %d", GetTimeString().c_str(), event->connect.status);
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "%s @ble_gap_event: BLE设备已连接，连接句柄: %d", GetTimeString().c_str(), event->connect.conn_handle);
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "%s @ble_gap_event: 连接设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                        GetTimeString().c_str(), desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], 
                        desc.peer_id_addr.val[3], desc.peer_id_addr.val[2], 
                        desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            } else {
                ESP_LOGW(TAG, "%s @ble_gap_event: 无法获取连接设备信息: %d", GetTimeString().c_str(), rc);
            }
            g_ble_config_instance->conn_handle_ = event->connect.conn_handle;
            ESP_LOGI(TAG, "%s @ble_gap_event: 保存连接句柄: %d", GetTimeString().c_str(), event->connect.conn_handle);
            g_ble_config_instance->StopAdvertising();
        } else {
            ESP_LOGW(TAG, "%s @ble_gap_event: 连接失败，重新开始广播", GetTimeString().c_str());
            g_ble_config_instance->StartAdvertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: DISCONNECT事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: BLE断开连接 - 原因: %d", GetTimeString().c_str(), event->disconnect.reason);
        g_ble_config_instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "%s @ble_gap_event: 连接已断开，重新开始广播", GetTimeString().c_str());
        g_ble_config_instance->StartAdvertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: ADV_COMPLETE事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: BLE广播完成事件 - 状态: %d", GetTimeString().c_str(), event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: MTU事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: MTU交换事件 - 连接句柄: %d, MTU: %d", GetTimeString().c_str(), event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: CONN_UPDATE事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: 连接参数更新事件 - 连接句柄: %d, 状态: %d", GetTimeString().c_str(), event->conn_update.conn_handle, event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (!g_ble_config_instance) {
            ESP_LOGE(TAG, "%s @ble_gap_event: SUBSCRIBE事件处理失败：全局实例不存在", GetTimeString().c_str());
            assert(g_ble_config_instance != nullptr);
            return 0;
        }
        ESP_LOGI(TAG, "%s @ble_gap_event: BLE订阅事件 - 连接句柄: %d, 属性句柄: %d, 订阅状态: %d", 
                GetTimeString().c_str(), event->subscribe.conn_handle, event->subscribe.attr_handle, 
                event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == g_ble_config_instance->status_val_handle_ &&
            event->subscribe.cur_notify) {
            ESP_LOGI(TAG, "%s @ble_gap_event: 客户端已订阅状态通知，发送初始状态", GetTimeString().c_str());
            g_ble_config_instance->SendWifiStatus(WIFI_STATUS_IDLE);
        }
        return 0;

    default:
        ESP_LOGD(TAG, "%s @ble_gap_event: 未处理的BLE事件: %d", GetTimeString().c_str(), event->type);
        return 0;
    }
}

void BleConfig::Deinitialize() {
    ESP_LOGI(TAG, "%s @Deinitialize: 开始完整去初始化BLE模块...", GetTimeString().c_str());
    
    // 记录初始内存状态
    MemorySnapshot initial_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "Deinitialize: 初始内存状态", initial_snapshot);

    // 1. 先停止所有BLE活动
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤1 - 停止所有BLE活动", GetTimeString().c_str());
    StopAdvertising();                  // 停止广播
    vTaskDelay(pdMS_TO_TICKS(100));     // 等待一段时间，确保广播已停止
    ESP_LOGI(TAG, "%s @Deinitialize: 广播已停止", GetTimeString().c_str());

    // 2. 设置任务状态为停止中
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤2 - 设置任务状态", GetTimeString().c_str());
    ble_host_task_state = BLE_TASK_STOPPING;
    ESP_LOGI(TAG, "%s @Deinitialize: 任务状态已设置为STOPPING", GetTimeString().c_str());
    
    // 3. 从看门狗中移除任务，标记任务需要退出
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤3 - 从看门狗移除任务并标记退出", GetTimeString().c_str());
    if (ble_host_task_handle != NULL) {
        esp_err_t wdt_err = esp_task_wdt_delete(ble_host_task_handle);
        if (wdt_err == ESP_OK) {
            ESP_LOGI(TAG, "%s @Deinitialize: BLE主机任务已成功从看门狗移除", GetTimeString().c_str());
        } else {
            ESP_LOGW(TAG, "%s @Deinitialize: 从看门狗移除BLE主机任务失败, 错误码: %d", GetTimeString().c_str(), wdt_err);
        }
    } else {
        ESP_LOGW(TAG, "%s @Deinitialize: BLE主机任务句柄为NULL，无需从看门狗移除", GetTimeString().c_str());
    }
    
    ble_host_task_running = false;
    ESP_LOGI(TAG, "%s @Deinitialize: BLE主机任务运行标志 (ble_host_task_running) 已设置为 false", GetTimeString().c_str());

    // 4. 等待任务退出，带超时机制
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤4 - 等待BLE主机任务退出", GetTimeString().c_str());
    const TickType_t xMaxWaitTicks = pdMS_TO_TICKS(3000);   // 3秒, 等待任务退出的最长时间
    TickType_t xStartTicks = xTaskGetTickCount();           // 记录任务开始的时间
    
    int wait_count = 0;  // 等待计数器，用于控制日志输出频率
    bool task_exited_normally = false; // 标志任务是否正常退出
    while (ble_host_task_state != BLE_TASK_STOPPED) {
        vTaskDelay(pdMS_TO_TICKS(10));  // 等待一段时间, 避免长时间占用CPU
        wait_count++;
        
        // 每100ms输出一次日志
        if (wait_count % 10 == 0) {
            int elapsed_ms = (int)((xTaskGetTickCount() - xStartTicks) * portTICK_PERIOD_MS);
            ESP_LOGI(TAG, "%s @Deinitialize: 等待任务退出中... (已等待 %d ms, 最大 %d ms, ble_host_task_state: %d)", 
                     GetTimeString().c_str(), elapsed_ms, (int)(xMaxWaitTicks * portTICK_PERIOD_MS), ble_host_task_state);
            
            // 检查任务状态
            if (ble_host_task_handle != NULL) {
                eTaskState task_state_freertos = eTaskGetState(ble_host_task_handle);
                ESP_LOGI(TAG, "%s @Deinitialize: FreeRTOS任务状态: %d", GetTimeString().c_str(), task_state_freertos);
            } else {
                ESP_LOGW(TAG, "%s @Deinitialize: ble_host_task_handle is NULL,无法获取FreeRTOS任务状态", GetTimeString().c_str());
            }
        }
        
        // 检查是否超时
        if ((xTaskGetTickCount() - xStartTicks) > xMaxWaitTicks) {  // 如果超过最大等待时间
            ESP_LOGW(TAG, "%s @Deinitialize: 等待ble_host_task退出超时 (ble_host_task_state: %d). nimble_port_run()可能阻塞. 尝试强制结束...", GetTimeString().c_str(), ble_host_task_state);
            
            if (ble_host_task_handle != NULL) {
                // 获取任务状态, 以确保任务已被删除
                eTaskState task_state_freertos = eTaskGetState(ble_host_task_handle);
                ESP_LOGI(TAG, "%s @Deinitialize: 超时前FreeRTOS任务状态: %d", GetTimeString().c_str(), task_state_freertos);
                
                // 强制删除任务
                vTaskDelete(ble_host_task_handle);  // 强制删除任务
                ESP_LOGI(TAG, "%s @Deinitialize: 已调用vTaskDelete强制删除ble_host_task", GetTimeString().c_str());
                
                ble_host_task_handle = NULL;                // 重置任务句柄
                ble_host_task_state = BLE_TASK_STOPPED;     // 设置任务状态为已停止
                task_exited_normally = false; // 标记为异常退出
            } else {
                ESP_LOGW(TAG, "%s @Deinitialize: 任务句柄为NULL，但ble_host_task_state (%d) 未设置为STOPPED. 强制设置为STOPPED.", GetTimeString().c_str(), ble_host_task_state);
                ble_host_task_state = BLE_TASK_STOPPED;     // 强制设置任务状态为已停止
                task_exited_normally = false; // 标记为异常退出
            }
            break;
        }
        if (ble_host_task_state == BLE_TASK_STOPPED) { // 检查任务是否在此轮循环中停止
            task_exited_normally = true;
        }
    }

    if (task_exited_normally) {
        ESP_LOGI(TAG, "%s @Deinitialize: BLE主机任务已正常退出 (ble_host_task_state: %d)", GetTimeString().c_str(), ble_host_task_state);
    } else {
        ESP_LOGW(TAG, "%s @Deinitialize: BLE主机任务未能正常退出或超时后被强制结束 (ble_host_task_state: %d). nimble_port_run()可能阻塞.", GetTimeString().c_str(), ble_host_task_state);
    }
    
    // 记录任务退出后的内存状态
    MemorySnapshot after_task = get_memory_snapshot();
    log_memory_state(TAG, "Deinitialize: BLE主机任务退出后内存状态", after_task);
    ESP_LOGI(TAG, "%s @Deinitialize: 释放: %d字节", 
             GetTimeString().c_str(), (int)(after_task.total_heap - initial_snapshot.total_heap));

    // 5. 去初始化NimBLE，添加重试机制
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤5 - 去初始化NimBLE", GetTimeString().c_str());
    const int MAX_DEINIT_RETRIES = 3;   // 最大重试次数
    int deinit_retry_count = 0;         // 当前重试次数
    int rc;
    
    // 循环尝试去初始化NimBLE
    do {
        ESP_LOGI(TAG, "%s @Deinitialize: 尝试去初始化NimBLE，第%d次", 
                 GetTimeString().c_str(), deinit_retry_count + 1);
        
        ESP_LOGI(TAG, "%s @Deinitialize: 调用 nimble_port_stop()", GetTimeString().c_str());
        nimble_port_stop(); // 停止NimBLE端口事件处理
        ESP_LOGI(TAG, "%s @Deinitialize: nimble_port_stop() 调用完成", GetTimeString().c_str());

        // 在 nimble_port_deinit() 之前调用 ble_hs_deinit()
        // 确保在尝试 deinit port 之前，host stack 已经被 deinit
        ESP_LOGI(TAG, "%s @Deinitialize: 调用 ble_hs_deinit() (在 nimble_port_deinit() 之前)", GetTimeString().c_str());
        ble_hs_deinit(); 
        ESP_LOGI(TAG, "%s @Deinitialize: ble_hs_deinit() 调用完成", GetTimeString().c_str());

        rc = nimble_port_deinit();  // 去初始化NimBLE
        if (rc == 0) {
            ESP_LOGI(TAG, "%s @Deinitialize: NimBLE模块去初始化成功", GetTimeString().c_str());
            break;
        } else if (rc == BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "%s @Deinitialize: NimBLE模块已去初始化 (BLE_HS_EALREADY)，无需重复操作", GetTimeString().c_str());
            rc = 0; // 视为成功
            break;
        }
        
        ESP_LOGW(TAG, "%s @Deinitialize: NimBLE模块去初始化失败: %d，重试次数: %d/%d", 
                 GetTimeString().c_str(), rc, deinit_retry_count + 1, MAX_DEINIT_RETRIES);
        
        // 短暂延时后重试
        vTaskDelay(pdMS_TO_TICKS(100));
        deinit_retry_count++;
        
    } while (deinit_retry_count < MAX_DEINIT_RETRIES);

    // 记录去初始化后的内存状态
    MemorySnapshot after_deinit = get_memory_snapshot();
    log_memory_state(TAG, "Deinitialize: nimble_port_deinit()后内存状态", after_deinit);
    ESP_LOGI(TAG, "%s @Deinitialize: nimble_port_deinit释放: %d字节", 
             GetTimeString().c_str(), (int)(after_deinit.total_heap - after_task.total_heap));

    // 6. 如果去初始化失败，记录错误并尝试强制清理
    if (rc != 0) {
        ESP_LOGE(TAG, "%s @Deinitialize: 步骤6 - NimBLE模块去初始化失败，进行强制清理", GetTimeString().c_str());
        
        // 强制停止广播
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "%s @Deinitialize: 强制停止广播成功", GetTimeString().c_str());
        } else {
            ESP_LOGI(TAG, "%s @Deinitialize: 广播已停止，无需强制停止", GetTimeString().c_str());
        }

        // 强制断开所有连接
        ESP_LOGI(TAG, "%s @Deinitialize: 开始断开所有连接", GetTimeString().c_str());
        int disconnect_count = 0;
        for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {    // 遍历所有连接，i为连接句柄
            int rc = ble_gap_terminate(i, BLE_ERR_REM_USER_CONN_TERM);  // 强制断开连接，错误码为用户终止
            if (rc == 0) {
                disconnect_count++;
            }
        }
        ESP_LOGI(TAG, "%s @Deinitialize: 已断开 %d 个连接", GetTimeString().c_str(), disconnect_count);

        // 重置关键状态变量
        ble_host_task_running = false;
        ble_host_task_state = BLE_TASK_STOPPED;
        ESP_LOGI(TAG, "%s @Deinitialize: 已重置任务状态变量", GetTimeString().c_str());
        
        // 如果任务句柄存在，强制删除任务
        if (ble_host_task_handle != nullptr) {
            vTaskDelete(ble_host_task_handle);
            ble_host_task_handle = nullptr;
            ESP_LOGI(TAG, "%s @Deinitialize: 已强制删除BLE主机任务", GetTimeString().c_str());
        }

        // 清空接收的WiFi凭据
        received_ssid_.clear();
        received_password_.clear();
        ESP_LOGI(TAG, "%s @Deinitialize: 已清空接收的WiFi凭据", GetTimeString().c_str());
        
        ESP_LOGI(TAG, "%s @Deinitialize: 紧急资源清理完成", GetTimeString().c_str());
    }

    // 7. 释放资源（即使去初始化失败也需要执行）
    ESP_LOGI(TAG, "%s @Deinitialize: 步骤7 - 最终资源释放检查", GetTimeString().c_str());
    if (ble_host_task_handle != NULL) {
        ESP_LOGW(TAG, "%s @Deinitialize: BLE主机任务句柄在最终清理步骤仍不为NULL，尝试再次删除", GetTimeString().c_str());
        vTaskDelete(ble_host_task_handle);
        ble_host_task_handle = NULL;
        ESP_LOGI(TAG, "%s @Deinitialize: 最终释放BLE主机任务资源完成", GetTimeString().c_str());
    } else {
        ESP_LOGI(TAG, "%s @Deinitialize: BLE主机任务句柄已为NULL，无需在最终步骤释放", GetTimeString().c_str());
    }

    // 清空全局实例指针，防止野指针
    g_ble_config_instance = nullptr;
    ESP_LOGI(TAG, "%s @Deinitialize: 已清空全局实例指针", GetTimeString().c_str());
    
    // 记录最终内存状态
    MemorySnapshot final_snapshot = get_memory_snapshot();
    log_memory_state(TAG, "Deinitialize: 完成时最终内存状态", final_snapshot);
    ESP_LOGI(TAG, "%s @Deinitialize: Deinitialize总释放: %d字节", 
             GetTimeString().c_str(), (int)(final_snapshot.total_heap - initial_snapshot.total_heap));
    
    ESP_LOGI(TAG, "%s @Deinitialize: BLE模块去初始化完成", GetTimeString().c_str());
}