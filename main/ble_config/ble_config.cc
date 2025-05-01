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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "[%H:%M:%S.", tm_info);
    // 添加毫秒部分（确保有足够空间：最多3位数字+']'+\0 = 5字节）
    size_t len = strlen(buffer);
    snprintf(buffer + len, sizeof(buffer) - len, "%03ld]", tv.tv_usec / 1000);
    return std::string(buffer);
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
    ESP_LOGI(TAG, "%s 开始解析UUID...", GetTimeString().c_str());

    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_svc_wifi_config_uuid.u, WIFI_CONFIG_SERVICE_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_ssid_uuid.u, SSID_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_password_uuid.u, PASSWORD_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_control_status_uuid.u, CONTROL_STATUS_CHAR_UUID);
    assert(rc == 0);
    
    ESP_LOGI(TAG, "%s UUID解析完成", GetTimeString().c_str());
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
        ESP_LOGI(TAG, "registered service %s with handle=0x%04x",
               ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
               ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG, "registering characteristic %s with def_handle=0x%04x val_handle=0x%04x",
               ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
               ctxt->chr.def_handle,
               ctxt->chr.val_handle);
               
        if (g_ble_config_instance && 
            ble_uuid_cmp(ctxt->chr.chr_def->uuid, &gatt_svr_chr_control_status_uuid.u) == 0) {
            g_ble_config_instance->status_val_handle_ = ctxt->chr.val_handle;
            ESP_LOGI(TAG, "保存控制状态特征值句柄: 0x%04x", ctxt->chr.val_handle);
        }
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGI(TAG, "registering descriptor %s with handle=0x%04x",
               ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
               ctxt->dsc.handle);
        break;
    default:
        assert(0);
        break;
    }
}

void BleConfig::Initialize() {
    g_ble_config_instance = this;   // 保存实例指针
    ESP_LOGI(TAG, "%s 开始初始化BLE配网模块...", GetTimeString().c_str());

    // 完全避免尝试清理资源的过程，直接初始化
    ESP_LOGI(TAG, "%s 跳过清理步骤，直接初始化NimBLE", GetTimeString().c_str());

    // 1. 初始化NVS
    ESP_LOGI(TAG, "%s 初始化NVS存储...", GetTimeString().c_str());
    esp_err_t nvs_ret = nvs_flash_init();

    // 如果NVS分区已满或版本不匹配，则擦除重新初始化
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s NVS需要擦除", GetTimeString().c_str());
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "%s NVS初始化失败: %s", GetTimeString().c_str(), esp_err_to_name(nvs_ret));
        return;
    }
    ESP_LOGI(TAG, "%s NVS初始化成功", GetTimeString().c_str());

    // 2. 解析服务和特征UUID
    ESP_LOGI(TAG, "%s 解析BLE服务UUID", GetTimeString().c_str());
    parse_all_uuids();

    // 3. 打印当前内存状态
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s BLE初始化前可用堆内存: %d 字节", GetTimeString().c_str(), free_heap);

    // 检查内存是否足够
    if (free_heap < 60000) {
        ESP_LOGW(TAG, "%s 可用内存较低，但仍将继续初始化", GetTimeString().c_str());
    }

    // 4. 初始化NimBLE
    ESP_LOGI(TAG, "%s 初始化NimBLE端口", GetTimeString().c_str());
    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "%s NimBLE端口初始化失败: %d", GetTimeString().c_str(), rc);
        return;
    }
    ESP_LOGI(TAG, "%s NimBLE端口初始化成功", GetTimeString().c_str());

    // 5. 配置BLE安全参数
    ESP_LOGI(TAG, "%s 配置BLE安全参数", GetTimeString().c_str());
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    // 6. 设置回调函数
    ESP_LOGI(TAG, "%s 设置BLE回调函数", GetTimeString().c_str());
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = NULL;

    // 7. 初始化GATT服务器
    ESP_LOGI(TAG, "%s 初始化GATT服务器", GetTimeString().c_str());
    gatt_svr_init();

    // 8. 设置设备名称
    ESP_LOGI(TAG, "%s 设置BLE设备名称", GetTimeString().c_str());
    int name_rc = ble_svc_gap_device_name_set("DuDu-BLE");
    if (name_rc != 0) {
        ESP_LOGW(TAG, "%s 设置设备名称失败: %d", GetTimeString().c_str(), name_rc);
    }

    // 9. 初始化BLE存储
    ESP_LOGI(TAG, "%s 初始化BLE存储", GetTimeString().c_str());
    ble_store_config_init();

    // 10. 启动BLE主机任务
    ESP_LOGI(TAG, "%s 创建BLE主机任务", GetTimeString().c_str());
    xTaskCreatePinnedToCore(
        ble_host_task, 
        "ble_host_task",
        16384,        // 16KB栈大小
        NULL,         // 不传递参数
        10,           // 改回 10
        &ble_host_task_handle, // <<< 保存任务句柄到成员变量
        0             // 在核心0上运行
    );

    // 等待一小段时间，让BLE任务启动
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "%s BLE初始化完成", GetTimeString().c_str());
}

int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char* char_name = (const char*)arg;
    int rc = 0;

    if (!g_ble_config_instance) {
        ESP_LOGE(TAG, "%s GATT访问失败: 全局实例不存在", GetTimeString().c_str());
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
            ESP_LOGI(TAG, "%s 收到特征值写入请求: %s, 数据长度: %d", GetTimeString().c_str(), char_name, data_len);

            // ==== 新增数据校验开始 ====
            // 控制命令必须为1字节
            if(strcmp(char_name, "control") == 0 && data_len != CONTROL_CMD_LEN) {
                ESP_LOGE(TAG, "%s 控制命令长度无效: %d, 应为1字节", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // SSID最大32字节
            if(strcmp(char_name, "ssid") == 0 && data_len > MAX_SSID_LEN) {
                ESP_LOGE(TAG, "%s SSID长度过长: %d/32", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; 
            }
            // 密码最大64字节
            if(strcmp(char_name, "password") == 0 && data_len > MAX_PASSWORD_LEN) {
                ESP_LOGE(TAG, "%s 密码长度过长: %d/64", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // ==== 新增数据校验结束 ====

            if (data_len > 256) {   // 原有长度检查
                ESP_LOGE(TAG, "%s 数据长度超过最大限制: %d/256", GetTimeString().c_str(), data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            uint8_t* data = (uint8_t*)malloc(data_len + 1);
            if (!data) {
                ESP_LOGE(TAG, "%s 内存分配失败，无法处理接收数据", GetTimeString().c_str());
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "%s 数据转换失败: %d", GetTimeString().c_str(), rc);
                free(data);
                return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0';

            ESP_LOGI(TAG, "%s 接收到 %d 字节数据: %.*s (十六进制: %s)", 
                    GetTimeString().c_str(), data_len, data_len, data, bytes_to_hex(data, data_len));

            if (char_name) {
                if (strcmp(char_name, "ssid") == 0) {
                    g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "%s 保存SSID: %s", GetTimeString().c_str(), g_ble_config_instance->received_ssid_.c_str());
                } else if (strcmp(char_name, "password") == 0) {
                    g_ble_config_instance->received_password_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "%s 保存密码: %s", GetTimeString().c_str(), g_ble_config_instance->received_password_.c_str());
                } else if (strcmp(char_name, "control") == 0 && 
                          data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {
                    ESP_LOGI(TAG, "%s 收到连接WiFi命令", GetTimeString().c_str());
                    if (!g_ble_config_instance->received_ssid_.empty() && 
                        !g_ble_config_instance->received_password_.empty()) {
                        ESP_LOGI(TAG, "%s SSID和密码已接收，准备连接WiFi", GetTimeString().c_str());
                        if (g_ble_config_instance->credentials_received_cb_) {
                            ESP_LOGI(TAG, "%s 调用凭据接收回调", GetTimeString().c_str());
                            g_ble_config_instance->credentials_received_cb_(
                                g_ble_config_instance->received_ssid_,
                                g_ble_config_instance->received_password_);
                        }
                        if (g_ble_config_instance->connect_wifi_cb_) {
                            ESP_LOGI(TAG, "%s 调用WiFi连接回调", GetTimeString().c_str());
                            g_ble_config_instance->connect_wifi_cb_();
                        }
                    } else {
                        ESP_LOGW(TAG, "%s 收到连接命令但SSID或密码为空", GetTimeString().c_str());
                    }
                }
            }
            free(data);
            break;
        }
        default:
            ESP_LOGW(TAG, "%s 不支持的GATT操作: %d", GetTimeString().c_str(), ctxt->op);
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return rc;
}

void BleConfig::gatt_svr_init(void) {
    ESP_LOGI(TAG, "%s 初始化GATT服务器...", GetTimeString().c_str());
    int rc;
    
    // 配置GATT服务
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s GATT服务计数配置失败: %d", GetTimeString().c_str(), rc);
        return; // 失败时直接返回，不使用assert
    }
    
    // 添加GATT服务
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s 添加GATT服务失败: %d", GetTimeString().c_str(), rc);
        return; // 失败时直接返回，不使用assert
    }
    
    ESP_LOGI(TAG, "%s GATT服务器初始化成功", GetTimeString().c_str());
}

void BleConfig::ble_advertise(void) {
    ESP_LOGI(TAG, "%s 准备开始BLE广播...", GetTimeString().c_str());
    const int MAX_RETRY = 3;  // 最大重试次数
    int retry_count = 0;
    
    // 先检查是否已有广播在运行，如果有则先停止
    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "%s 检测到广播已在运行，先停止当前广播", GetTimeString().c_str());
        int rc = ble_gap_adv_stop();
        if (rc != 0) {
            ESP_LOGW(TAG, "%s 停止当前广播失败: %d，继续尝试启动新广播", GetTimeString().c_str(), rc);
        } else {
            ESP_LOGI(TAG, "%s 已停止当前广播，准备启动新广播", GetTimeString().c_str());
        }
        // 添加短暂延时，确保BLE栈有足够时间处理停止操作
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    do {
        struct ble_gap_adv_params adv_params;   // 广播参数
        struct ble_hs_adv_fields fields;        // 广播字段
        
        memset(&fields, 0, sizeof fields);      // 清空广播字段
        
        // 使用最简单的广播字段配置
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;    // 广播类型
        
        // 设置设备名称
        const char* dev_name = ble_svc_gap_device_name();
        fields.name = (uint8_t *)dev_name;                                 // 广播名称
        fields.name_len = strlen(dev_name);                                // 广播名称长度
        fields.name_is_complete = 1;                                       // 使用完整名称

        ESP_LOGI(TAG, "%s 设置广播字段，设备名称: %s", GetTimeString().c_str(), dev_name);

        int rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "%s 设置广播字段失败: %d，重试次数: %d/%d", GetTimeString().c_str(), rc, retry_count + 1, MAX_RETRY);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1000));  // 延迟1秒后重试
            continue;
        }

        // 设置标准广播参数
        memset(&adv_params, 0, sizeof adv_params);                  // 清空广播参数
        adv_params.itvl_min = 32;  // 20ms (32 * 0.625ms)
        adv_params.itvl_max = 48;  // 30ms (48 * 0.625ms)
        adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;               // 连接模式
        adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;               // 发现模式

        ESP_LOGI(TAG, "%s 开始广播，间隔: %d-%d (单位: 0.625ms)", GetTimeString().c_str(), adv_params.itvl_min, adv_params.itvl_max);

        // 开始广播
        rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, ble_gap_event, NULL);

        if (rc == 0) {
            ESP_LOGI(TAG, "%s BLE广播已成功启动", GetTimeString().c_str());
            return;  // 广播成功，退出函数
        } else if (rc == BLE_HS_EALREADY) {
            // 错误码2表示已经有广播在运行
            ESP_LOGW(TAG, "%s 广播已在运行(BLE_HS_EALREADY)，尝试停止后重新启动", GetTimeString().c_str());
            ble_gap_adv_stop();
            vTaskDelay(pdMS_TO_TICKS(200));  // 给BLE栈一些时间处理停止操作
        } else {
            ESP_LOGE(TAG, "%s 启动BLE广播失败: %d，重试次数: %d/%d", GetTimeString().c_str(), rc, retry_count + 1, MAX_RETRY);
        }
        
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));  // 延迟1秒后重试
        
    } while (retry_count < MAX_RETRY);
    
    ESP_LOGE(TAG, "%s BLE广播启动失败，已达到最大重试次数", GetTimeString().c_str());
}

bool BleConfig::StartAdvertising() {
    ESP_LOGI(TAG, "%s 尝试开始BLE广播...", GetTimeString().c_str());
    
    // 检查BLE主机是否已同步
    if (ble_hs_synced()) {
        ESP_LOGI(TAG, "%s BLE主机已同步，准备开始广播", GetTimeString().c_str());
        
        // 检查是否已有广播在运行
        if (ble_gap_adv_active()) {
            ESP_LOGI(TAG, "%s 检测到广播已在运行，先停止当前广播", GetTimeString().c_str());
            int rc = ble_gap_adv_stop();
            if (rc != 0) {
                ESP_LOGW(TAG, "%s 停止当前广播失败: %d，但仍将尝试启动新广播", GetTimeString().c_str(), rc);
            } else {
                ESP_LOGI(TAG, "%s 已停止当前广播", GetTimeString().c_str());
            }
            // 添加短暂延时，确保BLE栈有足够时间处理停止操作
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 启动广播
        ble_advertise();
        return true; // 广播启动成功
    } else {
        ESP_LOGW(TAG, "%s BLE主机尚未同步，将在同步后自动开始广播", GetTimeString().c_str());
        return false; // 当前无法启动广播，需等待同步
    }
}

void BleConfig::StopAdvertising() {
    ESP_LOGI(TAG, "%s 尝试停止BLE广播...", GetTimeString().c_str());
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            ESP_LOGI(TAG, "%s BLE广播已成功停止", GetTimeString().c_str());
        } else {
            ESP_LOGE(TAG, "%s 停止BLE广播失败: %d", GetTimeString().c_str(), rc);
        }
    } else {
        ESP_LOGI(TAG, "%s BLE广播已经处于停止状态", GetTimeString().c_str());
    }
}

void BleConfig::ble_on_sync(void) {
    ESP_LOGI(TAG, "%s BLE主机同步完成，准备开始广播", GetTimeString().c_str());

    // 获取设备地址
    uint8_t addr_val[6] = {0};
    int rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "%s 设备MAC地址: %02x:%02x:%02x:%02x:%02x:%02x",
                 GetTimeString().c_str(), addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);
    } else {
        ESP_LOGE(TAG, "%s 获取设备地址失败: %d", GetTimeString().c_str(), rc);
    }

    // 开始广播
    if (g_ble_config_instance) {
        ESP_LOGI(TAG, "%s BLE实例已初始化，开始广播", GetTimeString().c_str());
        g_ble_config_instance->ble_advertise();
    } else {
        ESP_LOGE(TAG, "%s BLE实例未初始化，无法开始广播", GetTimeString().c_str());
    }
}

void BleConfig::ble_on_reset(int reason) {
    ESP_LOGE(TAG, "%s BLE主机重置，原因: %d", GetTimeString().c_str(), reason);
}

// BLE主机任务，负责管理BLE主机的生命周期，包括初始化、同步、广播等
void BleConfig::ble_host_task(void *param) {
    ESP_LOGI(TAG, "%s BLE主机任务已启动", GetTimeString().c_str());

    // 1. 保存任务句柄
    ble_host_task_handle = xTaskGetCurrentTaskHandle();

    // 2. 添加到看门狗
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));


    // esp_err_t err = esp_task_wdt_add(NULL);
    // if (err != ESP_OK) {
    //     ESP_LOGW(TAG, "%s 添加任务到看门狗失败: %d", GetTimeString().c_str(), err);
    // }



    // 3. 设置任务状态
    ble_host_task_state = BLE_TASK_RUNNING;

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s BLE主机任务启动时可用堆内存: %d 字节", GetTimeString().c_str(), free_heap);

    // 4. 主循环
    while (ble_host_task_running) {
        // 获取事件队列
        struct ble_npl_eventq *eventq = nimble_port_get_dflt_eventq();
        if (eventq != NULL) {
            // 处理事件，设置超时确保定期喂狗
            struct ble_npl_event *ev = ble_npl_eventq_get(eventq, pdMS_TO_TICKS(100));
            if (ev) {
                ble_npl_event_run(ev);
            }
        }
        
        // 喂狗
        esp_task_wdt_reset();
        
        // 短暂延时
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 5. 清理工作
    esp_task_wdt_delete(NULL);  // 从看门狗中移除任务
    ESP_LOGI(TAG, "%s BLE主机任务退出", GetTimeString().c_str());
    ble_host_task_state = BLE_TASK_STOPPED;
    vTaskDelete(NULL);







    // // 初始化主机
    // ESP_LOGI(TAG, "%s 初始化BLE主机", GetTimeString().c_str());
    // ble_hs_init();
    
    // // 获取事件队列
    // struct ble_npl_eventq *eventq = nimble_port_get_dflt_eventq();
    // if (eventq == NULL) {
    //     ESP_LOGE(TAG, "%s 获取事件队列失败", GetTimeString().c_str());
    //     goto cleanup;
    // }
    
    // // 主循环
    // ESP_LOGI(TAG, "%s 进入BLE事件循环", GetTimeString().c_str());
    // while (1) {
    //     // 阻塞等待BLE事件，设置1秒超时，以便定期喂狗
    //     struct ble_npl_event *ev = ble_npl_eventq_get(eventq, pdMS_TO_TICKS(1000));
        
    //     // 每次循环都喂狗
    //     esp_task_wdt_reset();

    //     // 处理事件
    //     if (ev) {
    //         ble_npl_event_run(ev);
    //     }
        
    //     // 短暂延时，避免CPU占用过高
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
    
// cleanup:
//     // 从看门狗中移除任务
//     esp_task_wdt_delete(NULL);
//     ESP_LOGI(TAG, "%s BLE主机任务退出", GetTimeString().c_str());
//     vTaskDelete(NULL);
}

void BleConfig::SendWifiStatus(wifi_config_status_t status) {
    ESP_LOGI(TAG, "%s 尝试发送WiFi状态: %d", GetTimeString().c_str(), status);
    
    // 喂任务看门狗，防止长时间操作导致看门狗超时
    esp_task_wdt_reset();
    
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || status_val_handle_ == 0) {
        ESP_LOGW(TAG, "%s 无法发送状态，没有连接或句柄无效 (conn_handle=%d, status_val_handle=%d)", 
                GetTimeString().c_str(), conn_handle_, status_val_handle_);
        return;
    }

    // 分配内存前先检查可用堆内存
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s 当前可用堆内存: %d 字节", GetTimeString().c_str(), free_heap);
    
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&status, sizeof(status));
    if (!om) {
        ESP_LOGE(TAG, "%s 为通知分配内存失败", GetTimeString().c_str());
        return;
    }

    // === 修正重试机制：只在循环内发送，不再多发一次 ===
    // 增加短暂延时，确保BLE栈有足够时间处理
    int rc = -1;
    for(int retry = 0; retry < NOTIFY_RETRY_COUNT; retry++) {
        // 每次发送前喂狗
        esp_task_wdt_reset();
        
        rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
        if(rc == 0) {
            ESP_LOGI(TAG, "%s WiFi状态通知发送成功: %d", GetTimeString().c_str(), status);
            break;
        }
        ESP_LOGW(TAG, "%s 通知发送失败 (尝试 %d/%d)，错误码: %d，稍后重试...", 
                GetTimeString().c_str(), retry+1, NOTIFY_RETRY_COUNT, rc);
        
        // 延时前再次喂狗
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "%s 发送通知失败，所有重试均失败; rc=%d", GetTimeString().c_str(), rc);
    }

    os_mbuf_free_chain(om);  // 确保释放mbuf
    
    // 操作完成后再次喂狗
    esp_task_wdt_reset();
}

void BleConfig::SetCredentialsReceivedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    ESP_LOGI(TAG, "设置凭据接收回调");
    credentials_received_cb_ = cb;
}

void BleConfig::SetConnectWifiCallback(std::function<void()> cb) {
    ESP_LOGI(TAG, "设置WiFi连接回调");
    connect_wifi_cb_ = cb;
}


int BleConfig::ble_gap_event(struct ble_gap_event *event, void *arg) {
    // 确保实例存在
    struct ble_gap_conn_desc desc;
    int rc;
    if (!g_ble_config_instance) {
        ESP_LOGE(TAG, "%s BLE事件处理失败：全局实例不存在", GetTimeString().c_str());
        return 0;
    }

    ESP_LOGD(TAG, "%s 收到BLE事件: %d", GetTimeString().c_str(), event->type);
    
    switch (event->type) {

    // === 新增加密状态处理 === 
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.status == 0) {
            ESP_LOGI(TAG, "%s 加密状态变更: %s", 
                    GetTimeString().c_str(), event->enc_change.status == 0 ? "已加密" : "未加密");
        } else {
            ESP_LOGE(TAG, "%s 加密失败, 状态码: %d", GetTimeString().c_str(), event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "%s BLE连接事件 - 状态: %d", GetTimeString().c_str(), event->connect.status);
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "%s BLE设备已连接，连接句柄: %d", GetTimeString().c_str(), event->connect.conn_handle);
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "%s 连接设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                        GetTimeString().c_str(), desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], 
                        desc.peer_id_addr.val[3], desc.peer_id_addr.val[2], 
                        desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            } else {
                ESP_LOGW(TAG, "%s 无法获取连接设备信息: %d", GetTimeString().c_str(), rc);
            }
            g_ble_config_instance->conn_handle_ = event->connect.conn_handle;
            ESP_LOGI(TAG, "%s 保存连接句柄: %d", GetTimeString().c_str(), event->connect.conn_handle);
            g_ble_config_instance->StopAdvertising();
        } else {
            ESP_LOGW(TAG, "%s 连接失败，重新开始广播", GetTimeString().c_str());
            g_ble_config_instance->StartAdvertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "%s BLE断开连接 - 原因: %d", GetTimeString().c_str(), event->disconnect.reason);
        g_ble_config_instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "%s 连接已断开，重新开始广播", GetTimeString().c_str());
        g_ble_config_instance->StartAdvertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "%s BLE广播完成事件 - 状态: %d", GetTimeString().c_str(), event->adv_complete.reason);
        // ESP_LOGI(TAG, "%s BLE广播完成事件 - 实例: %d, 状态: %d", GetTimeString().c_str(), event->adv_complete.instance, event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "%s BLE订阅事件 - 连接句柄: %d, 属性句柄: %d, 订阅状态: %d", 
                GetTimeString().c_str(), event->subscribe.conn_handle, event->subscribe.attr_handle, 
                event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == g_ble_config_instance->status_val_handle_ &&
            event->subscribe.cur_notify) {
            ESP_LOGI(TAG, "%s 客户端已订阅状态通知，发送初始状态", GetTimeString().c_str());
            g_ble_config_instance->SendWifiStatus(WIFI_STATUS_IDLE);
        }
        return 0;

    default:
        ESP_LOGD(TAG, "%s 未处理的BLE事件: %d", GetTimeString().c_str(), event->type);
        return 0;
    }
}

void BleConfig::Deinitialize() {
    ESP_LOGI(TAG, "%s @Deinitialize：开始完整去初始化BLE模块...", GetTimeString().c_str());

    // 1. 先停止所有BLE活动
    StopAdvertising();
    ESP_LOGI(TAG, "%s @Deinitialize：停止所有BLE活动完成...", GetTimeString().c_str());

    // 2. 等待所有BLE操作完成
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 3. 从看门狗中移除任务
    if (ble_host_task_handle != NULL) {
        esp_task_wdt_delete(ble_host_task_handle);
        ESP_LOGI(TAG, "%s @Deinitialize：从看门狗中移除BLE任务完成...", GetTimeString().c_str());
    }

    // 4. 标记任务需要退出
    ble_host_task_running = false;

    // 5. 等待任务自行退出，添加超时机制
    const TickType_t xMaxWaitTicks = pdMS_TO_TICKS(3000); // 最多等待3秒
    TickType_t xStartTicks = xTaskGetTickCount();
    
    while (ble_host_task_state != BLE_TASK_STOPPED) {
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // 检查是否超时
        if ((xTaskGetTickCount() - xStartTicks) > xMaxWaitTicks) {
            ESP_LOGW(TAG, "%s @Deinitialize：等待任务退出超时，尝试强制结束...", GetTimeString().c_str());
            
            if (ble_host_task_handle != NULL) {
                // 获取任务状态
                eTaskState task_state = eTaskGetState(ble_host_task_handle);
                ESP_LOGI(TAG, "%s @Deinitialize：任务当前状态: %d", GetTimeString().c_str(), task_state);
                
                // 强制删除任务
                vTaskDelete(ble_host_task_handle);
                ESP_LOGI(TAG, "%s @Deinitialize：已强制删除任务", GetTimeString().c_str());
                
                // 重置任务相关变量
                ble_host_task_handle = NULL;
                ble_host_task_state = BLE_TASK_STOPPED;
            }
            break;
        }
        
        ESP_LOGI(TAG, "%s @Deinitialize：等待BLE任务退出... (已等待 %d ms)", 
                 GetTimeString().c_str(), 
                 (int)((xTaskGetTickCount() - xStartTicks) * portTICK_PERIOD_MS));
    }
    
    ESP_LOGI(TAG, "%s @Deinitialize：BLE任务退出完成", GetTimeString().c_str());

    // 6. 去初始化NimBLE，添加重试机制
    const int MAX_DEINIT_RETRIES = 3;
    int deinit_retry_count = 0;
    int rc;
    
    do {
        rc = nimble_port_deinit();
        if (rc == 0) {
            ESP_LOGI(TAG, "%s @Deinitialize：NimBLE模块去初始化成功", GetTimeString().c_str());
            break;
        }
        
        ESP_LOGW(TAG, "%s @Deinitialize：NimBLE模块去初始化失败: %d，重试次数: %d/%d", 
                 GetTimeString().c_str(), rc, deinit_retry_count + 1, MAX_DEINIT_RETRIES);
        
        // 短暂延时后重试
        vTaskDelay(pdMS_TO_TICKS(100));
        deinit_retry_count++;
        
    } while (deinit_retry_count < MAX_DEINIT_RETRIES);

    // 如果去初始化失败，记录错误并尝试强制清理
    if (rc != 0) {
        ESP_LOGE(TAG, "%s @Deinitialize：NimBLE模块去初始化最终失败，将尝试强制清理资源", GetTimeString().c_str());
        
        // 这里可以添加紧急清理代码，比如重置关键状态变量
        // TODO: 根据具体项目需求添加额外的清理步骤
    }

    // 7. 释放资源（即使去初始化失败也需要执行）
    if (ble_host_task_handle != NULL) {
        vTaskDelete(ble_host_task_handle);
        ble_host_task_handle = NULL;
        ESP_LOGI(TAG, "%s @Deinitialize：释放BLE任务资源完成", GetTimeString().c_str());
    }

    // 清空全局实例指针，防止野指针
    g_ble_config_instance = nullptr; // <<< 置空，避免在 BLE 已经释放后，静态回调函数（如果被意外调用）使用无效的指针。
    ESP_LOGI(TAG, "%s @Deinitialize：BLE模块去初始化完成", GetTimeString().c_str());
}
