#include "ble_config.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static const char* TAG = "BLE_CONFIG";

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

    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_svc_wifi_config_uuid.u, WIFI_CONFIG_SERVICE_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_ssid_uuid.u, SSID_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_password_uuid.u, PASSWORD_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_control_status_uuid.u, CONTROL_STATUS_CHAR_UUID);
    assert(rc == 0);
}


// static const struct ble_gatt_chr_def gatt_svr_characteristics[] = {
//     {
//         /* 严格按照结构体定义顺序初始化 */
//         .uuid = &gatt_svr_chr_ssid_uuid.u,
//         .access_cb = BleConfig::gatt_svr_chr_access,
//         .arg = (void*)"ssid",
//         .descriptors = NULL,  // 明确设置为NULL
//         .flags = BLE_GATT_CHR_F_WRITE,
//         .min_key_size = 0,    
//         .val_handle = NULL,
//         .cpfd = NULL          
//     }, {
//         .uuid = &gatt_svr_chr_password_uuid.u,
//         .access_cb = BleConfig::gatt_svr_chr_access,
//         .arg = (void*)"password",
//         .descriptors = NULL,
//         .flags = BLE_GATT_CHR_F_WRITE,
//         .min_key_size = 0,
//         .val_handle = NULL,
//         .cpfd = NULL
//     }, {
//         .uuid = &gatt_svr_chr_control_status_uuid.u,
//         .access_cb = BleConfig::gatt_svr_chr_access,
//         .arg = (void*)"control",
//         .descriptors = NULL,
//         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
//         .min_key_size = 0,
//         .val_handle = &g_ble_config_instance->status_val_handle_,
//         .cpfd = NULL
//     },
//     { 0 } /* 结束标记 */
// };


static const struct ble_gatt_chr_def gatt_svr_characteristics[] = {
    // SSID特征值
    {
        .uuid = &gatt_svr_chr_ssid_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"ssid",
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = NULL,      // 显式初始化所有字段
        .min_key_size = 16,      // 建议安全密钥长度
        .descriptors = NULL,
        .cpfd = NULL
    },
    // Password特征值
    {
        .uuid = &gatt_svr_chr_password_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"password",
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = NULL,
        .min_key_size = 16,
        .descriptors = NULL,
        .cpfd = NULL
    },
    // Control特征值
    {
        .uuid = &gatt_svr_chr_control_status_uuid.u,
        .access_cb = BleConfig::gatt_svr_chr_access,
        .arg = (void*)"control",
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_ble_config_instance->status_val_handle_,
        .min_key_size = 16,
        .descriptors = NULL,
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
    g_ble_config_instance = this;
    ESP_LOGI(TAG, "Initializing BLE...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. GATT服务配置
    parse_all_uuids();
    
    // 1. NimBLE协议栈初始化
    nimble_port_init();

    // === 安全配置开始 ===
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    // === 安全配置结束 ===

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = NULL;

    // 新版API注册方式（ESP-IDF v5.3.2+）
    // 3. 设备名称设置
    static struct ble_gap_event_listener ble_event_listener = {0};
    int rc = ble_gap_event_listener_register(
        &ble_event_listener,
        ble_gap_event,  // 注意：这里不需要取地址符&
        NULL);          // 没有需要传递的参数
    assert(rc == 0);

    gatt_svr_init();

    rc = ble_svc_gap_device_name_set("DuDu-Alex");
    assert(rc == 0);


    // 4. Host任务启动
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE Initialized.");
}

int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char* char_name = (const char*)arg;
    int rc = 0;

    if (!g_ble_config_instance) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);

            // ==== 新增数据校验开始 ====
            // 控制命令必须为1字节
            if(strcmp(char_name, "control") == 0 && data_len != CONTROL_CMD_LEN) {
                ESP_LOGE(TAG, "Invalid control command length: %d", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // SSID最大32字节
            if(strcmp(char_name, "ssid") == 0 && data_len > MAX_SSID_LEN) {
                ESP_LOGE(TAG, "SSID too long: %d/32", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; 
            }
            // 密码最大64字节
            if(strcmp(char_name, "password") == 0 && data_len > MAX_PASSWORD_LEN) {
                ESP_LOGE(TAG, "Password too long: %d/64", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // ==== 新增数据校验结束 ====


            if (data_len > 256) {   // 原有长度检查
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            uint8_t* data = (uint8_t*)malloc(data_len + 1);
            if (!data) {
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                free(data);
                return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0';

            ESP_LOGD(TAG, "Received %d bytes: %.*s (Hex: %s)", 
                    data_len, data_len, data, bytes_to_hex(data, data_len));

            if (char_name) {
                if (strcmp(char_name, "ssid") == 0) {
                    g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                } else if (strcmp(char_name, "password") == 0) {
                    g_ble_config_instance->received_password_.assign((char*)data, data_len);
                } else if (strcmp(char_name, "control") == 0 && 
                          data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {
                    if (!g_ble_config_instance->received_ssid_.empty() && 
                        !g_ble_config_instance->received_password_.empty()) {
                        if (g_ble_config_instance->credentials_received_cb_) {
                            g_ble_config_instance->credentials_received_cb_(
                                g_ble_config_instance->received_ssid_,
                                g_ble_config_instance->received_password_);
                        }
                        if (g_ble_config_instance->connect_wifi_cb_) {
                            g_ble_config_instance->connect_wifi_cb_();
                        }
                    }
                }
            }
            free(data);
            break;
        }
        default:
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return rc;
}

void BleConfig::gatt_svr_init(void) {
    int rc;
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);
    ESP_LOGI(TAG, "GATT server initialized");
}

void BleConfig::ble_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;
    fields.uuids128 = &gatt_svr_svc_wifi_config_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return;

    memset(&adv_params, 0, sizeof adv_params);
    // === 新增广播间隔配置 ===
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL_MIN1; // 30ms (行240)
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL_MAX1; // 50ms (行241)
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                        &adv_params, ble_gap_event, NULL);

    if (rc == 0) {
        ESP_LOGI(TAG, "BLE advertising started");
    }
}

void BleConfig::StartAdvertising() {
    if (ble_hs_synced()) {
        ble_advertise();
    } else {
        ESP_LOGW(TAG, "BLE Host not synced yet, advertising will start on sync.");
    }
}

void BleConfig::StopAdvertising() {
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            ESP_LOGI(TAG, "BLE advertising stopped");
        } else {
            ESP_LOGE(TAG, "Failed to stop advertising; rc=%d", rc);
        }
    } else {
        ESP_LOGI(TAG, "BLE advertising already stopped");
    }
}

void BleConfig::ble_on_sync(void) {
    int rc;
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    ESP_LOGI(TAG, "BLE Host synced, starting advertising.");
    ble_advertise();
}

void BleConfig::ble_on_reset(int reason) {
    ESP_LOGE(TAG, "Resetting state; reason=%d", reason);
}

void BleConfig::ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "BLE Host Task Stopped");
}

void BleConfig::SendWifiStatus(wifi_config_status_t status) {
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || status_val_handle_ == 0) {
        ESP_LOGW(TAG, "Cannot send status, no connection or invalid handle");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&status, sizeof(status));
    if (!om) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for notification");
        return;
    }

    // === 新增重试机制开始 ===
    int rc = -1;
    for(int retry = 0; retry < NOTIFY_RETRY_COUNT; retry++) {  // 行320-321
        rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
        if(rc == 0) break;
        ESP_LOGW(TAG, "Notification failed (attempt %d), retrying...", retry+1);
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));  // 等待100ms重试
    }
    // === 新增重试机制结束 ===

    rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error sending notification; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Sent Wi-Fi status notification: %d", status);
    }

    os_mbuf_free_chain(om);  // 确保释放mbuf
}

void BleConfig::SetCredentialsReceivedCallback(std::function<void(const std::string&, const std::string&)> cb) {
    credentials_received_cb_ = cb;
}

void BleConfig::SetConnectWifiCallback(std::function<void()> cb) {
    connect_wifi_cb_ = cb;
}

int BleConfig::ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    int rc;

    if (!g_ble_config_instance) {
        return 0;
    }

    switch (event->type) {

    // === 新增加密状态处理 === 
    case BLE_GAP_EVENT_ENC_CHANGE:
    if(event->enc_change.status == 0) {
        ESP_LOGI(TAG, "加密状态变更: %s", 
                event->enc_change.status == 0 ? "已加密" : "未加密");
    } else {
        ESP_LOGE(TAG, "加密失败, 状态码: %d", event->enc_change.status);
    }
    return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            g_ble_config_instance->conn_handle_ = event->connect.conn_handle;
            g_ble_config_instance->StopAdvertising();
        } else {
            g_ble_config_instance->StartAdvertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_ble_config_instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        g_ble_config_instance->StartAdvertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_ble_config_instance->status_val_handle_ &&
            event->subscribe.cur_notify) {
            g_ble_config_instance->SendWifiStatus(WIFI_STATUS_IDLE);
        }
        return 0;

    default:
        return 0;
    }
}