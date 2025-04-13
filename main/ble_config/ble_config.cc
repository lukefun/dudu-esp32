#include "ble_config.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "string.h" // for memcpy

static const char* TAG = "BLE_CONFIG";

// --- UUID 解析 ---
// 辅助函数，将字符串 UUID 转换为 NimBLE 需要的格式
static int parse_uuid(const char *uuid_str, ble_uuid_any_t *uuid) {
    if (strlen(uuid_str) == 36) { // 128-bit UUID
        uuid->u.type = BLE_UUID_TYPE_128;
        return ble_uuid_from_str(uuid_str, &uuid->u128);
    } else if (strlen(uuid_str) == 4) { // 16-bit UUID
        uuid->u.type = BLE_UUID_TYPE_16;
        return ble_uuid16_from_str(uuid_str, &uuid->u16);
    }
    return -1;
}

// 全局变量，用于在回调中访问单例实例
static BleConfig* g_ble_config_instance = nullptr;

// --- GATT 服务定义 ---
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /* Service: Wi-Fi Configuration */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x63, 0xdb, 0x2d, 0x50, 0x90, 0xc0, 0x47, 0x8e,
                                    0x4d, 0x4d, 0xf1, 0x73, 0x0d, 0x95, 0xb7, 0xcd), // 使用解析后的 CDB7950D-73F1-4D4D-8E47-C090502DBD63
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* Characteristic: SSID */
                .uuid = BLE_UUID128_DECLARE(0x64, 0xdb, 0x2d, 0x50, 0x90, 0xc0, 0x47, 0x8e,
                                            0x4d, 0x4d, 0xf1, 0x73, 0x0d, 0x95, 0xb7, 0xcd), // CDB7950D-73F1-4D4D-8E47-C090502DBD64
                .access_cb = BleConfig::gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE, // 只允许写入
                .arg = (void*)"ssid"           // 用于回调中区分特征值
            }, {
                /* Characteristic: Password */
                .uuid = BLE_UUID128_DECLARE(0x65, 0xdb, 0x2d, 0x50, 0x90, 0xc0, 0x47, 0x8e,
                                            0x4d, 0x4d, 0xf1, 0x73, 0x0d, 0x95, 0xb7, 0xcd), // CDB7950D-73F1-4D4D-8E47-C090502DBD65
                .access_cb = BleConfig::gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE, // 只允许写入
                .arg = (void*)"password"        // 用于回调中区分特征值
            }, {
                /* Characteristic: Control/Status */
                 .uuid = BLE_UUID128_DECLARE(0x66, 0xdb, 0x2d, 0x50, 0x90, 0xc0, 0x47, 0x8e,
                                             0x4d, 0x4d, 0xf1, 0x73, 0x0d, 0x95, 0xb7, 0xcd), // CDB7950D-73F1-4D4D-8E47-C090502DBD66
                .access_cb = BleConfig::gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, // 允许写入和通知
                .val_handle = &g_ble_config_instance->status_val_handle_, // 将句柄保存到实例中
                .arg = (void*)"control"       // 用于回调中区分特征值
            },
            { 0 } /* 结束标记 */
        }
    },
    { 0 } /* 结束标记 */
};

// --- BleConfig 类成员函数实现 ---

void BleConfig::Initialize() {
    g_ble_config_instance = this; // 保存实例指针
    ESP_LOGI(TAG, "Initializing BLE...");

    // 初始化 NVS (NimBLE 需要)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化 NimBLE 协议栈
    nimble_port_init();

    // 配置 BLE Host 任务
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE Initialized.");
}

// --- 回调函数和静态函数实现 (暂时留空) ---
/*
int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // --- 这里将处理 GATT 读写请求 ---
    const char* char_name = (const char*)arg;
    ESP_LOGI(TAG, "GATT access event: op=%d, attr=0x%04x, characteristic=%s", ctxt->op, attr_handle, char_name);

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            ESP_LOGI(TAG, "Write operation on handle 0x%x", attr_handle);
            // 读取写入的数据
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t* data = (uint8_t*)malloc(data_len + 1); // +1 for null terminator
             if (!data) {
                 ESP_LOGE(TAG, "Failed to allocate memory for write data");
                 return BLE_ATT_ERR_INSUFFICIENT_RES;
             }
            int rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                 ESP_LOGE(TAG, "Failed to read mbuf data, rc=%d", rc);
                 free(data);
                 return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0'; // Add null terminator for string safety

            ESP_LOGI(TAG, "Received %d bytes: %.*s (Hex: %s)", data_len, data_len, data, util_bytes_to_hex(data, data_len));


            if (strcmp(char_name, "ssid") == 0) {
                g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                ESP_LOGI(TAG, "Received SSID: %s", g_ble_config_instance->received_ssid_.c_str());
            } else if (strcmp(char_name, "password") == 0) {
                g_ble_config_instance->received_password_.assign((char*)data, data_len);
                 ESP_LOGI(TAG, "Received Password: %s", g_ble_config_instance->received_password_.c_str());
                // // 密码接收后，可以触发配网回调（如果SSID也已接收）
                // if (!g_ble_config_instance->received_ssid_.empty() && g_ble_config_instance->credentials_received_cb_) {
                //     g_ble_config_instance->credentials_received_cb_(g_ble_config_instance->received_ssid_, g_ble_config_instance->received_password_);
                // }
            } else if (strcmp(char_name, "control") == 0) {
                if (data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {
                    ESP_LOGI(TAG, "Received Connect command");
                    // 收到连接命令
                     if (!g_ble_config_instance->received_ssid_.empty() && !g_ble_config_instance->received_password_.empty() && g_ble_config_instance->credentials_received_cb_) {
                         g_ble_config_instance->credentials_received_cb_(g_ble_config_instance->received_ssid_, g_ble_config_instance->received_password_);
                    } else if (g_ble_config_instance->connect_wifi_cb_) {
                         g_ble_config_instance->connect_wifi_cb_();
                     }
                } else {
                    ESP_LOGW(TAG, "Received unknown control command or invalid length: 0x%02X, len=%d", data[0], data_len);
                }
            }
             free(data);
            return 0; // Success
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}
*/



// --- BleConfig 类成员函数实现 ---

// --- GATT 特征值访问回调 ---
int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char* char_name = (const char*)arg; // 获取我们在服务定义中设置的特征值标识符
    int rc = 0; // 返回码，0 表示成功

    ESP_LOGD(TAG, "GATT access: op=%d, attr=0x%04x, characteristic=%s, conn_handle=0x%x",
             ctxt->op, attr_handle, char_name ? char_name : "unknown", conn_handle);

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: { // 处理写操作
            ESP_LOGI(TAG, "Write operation on '%s'", char_name ? char_name : "unknown");

            // 检查实例指针是否有效
            if (!g_ble_config_instance) {
                ESP_LOGE(TAG, "BleConfig instance is null!");
                return BLE_ATT_ERR_UNLIKELY;
            }

            // 检查连接句柄是否匹配 (可选，增加安全性)
            if (conn_handle != g_ble_config_instance->conn_handle_) {
                ESP_LOGW(TAG, "Write request from unexpected connection handle: 0x%x (expected 0x%x)",
                         conn_handle, g_ble_config_instance->conn_handle_);
                // return BLE_ATT_ERR_WRITE_NOT_PERMITTED; // 可以选择拒绝非当前连接的写入
            }

            // 读取写入的数据
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
            if (data_len > 256) { // 添加长度检查，防止过大的数据包
                 ESP_LOGE(TAG, "Write data too long: %d bytes", data_len);
                 return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // 分配内存并读取数据
            uint8_t* data = (uint8_t*)malloc(data_len + 1); // +1 for null terminator
            if (!data) {
                ESP_LOGE(TAG, "Failed to allocate memory for write data (%d bytes)", data_len);
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to read mbuf data, rc=%d", rc);
                free(data);
                return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0'; // 添加空终止符，方便字符串处理

            ESP_LOGD(TAG, "Received %d bytes: %.*s (Hex: %s)", data_len, data_len, data, util_bytes_to_hex(data, data_len));

            // 根据特征值标识符处理数据
            if (char_name != nullptr) {
                if (strcmp(char_name, "ssid") == 0) {
                    g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "Received SSID: %s", g_ble_config_instance->received_ssid_.c_str());
                } else if (strcmp(char_name, "password") == 0) {
                    g_ble_config_instance->received_password_.assign((char*)data, data_len);
                    // 不打印密码到日志
                    ESP_LOGI(TAG, "Received Password (length: %d)", data_len);
                } else if (strcmp(char_name, "control") == 0) {
                    if (data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {
                        ESP_LOGI(TAG, "Received Connect command");
                        // 触发连接 Wi-Fi 的回调
                        if (g_ble_config_instance->connect_wifi_cb_) {
                            // 在这里检查SSID和密码是否都已接收
                            if (!g_ble_config_instance->received_ssid_.empty() && !g_ble_config_instance->received_password_.empty()) {
                                 // 调用保存凭据的回调（如果有的话）
                                if (g_ble_config_instance->credentials_received_cb_) {
                                     g_ble_config_instance->credentials_received_cb_(
                                         g_ble_config_instance->received_ssid_,
                                         g_ble_config_instance->received_password_);
                                 }
                                // 调用开始连接的回调
                                g_ble_config_instance->connect_wifi_cb_();
                                // 清空已接收的凭据，为下次配网准备
                                g_ble_config_instance->received_ssid_.clear();
                                g_ble_config_instance->received_password_.clear();
                            } else {
                                ESP_LOGE(TAG, "Connect command received, but SSID or Password is missing!");
                                rc = BLE_ATT_ERR_INVALID_PDU; // 返回错误，告知手机端缺少信息
                            }
                        } else {
                            ESP_LOGW(TAG, "Connect command received, but no connect callback is set!");
                        }
                    } else {
                        ESP_LOGW(TAG, "Received unknown control command or invalid length: 0x%02X, len=%d", (data_len > 0 ? data[0] : 0), data_len);
                        rc = BLE_ATT_ERR_INVALID_PDU; // 无效的 PDU
                    }
                } else {
                    ESP_LOGW(TAG, "Write to unknown characteristic: %s", char_name);
                    rc = BLE_ATT_ERR_ATTR_NOT_FOUND; // 属性未找到
                }
            } else {
                 ESP_LOGE(TAG, "Characteristic argument (name) is null!");
                 rc = BLE_ATT_ERR_UNLIKELY;
            }

            free(data); // 释放内存
            return rc; // 返回处理结果
        }

        // NimBLE 不需要显式处理读操作，因为它会直接从属性数据库返回值
        // 如果需要动态生成读取的值，则需要处理 BLE_GATT_ACCESS_OP_READ_CHR
        case BLE_GATT_ACCESS_OP_READ_CHR: {
            ESP_LOGI(TAG, "Read operation on handle 0x%x ('%s') - Not implemented for write-only chars", attr_handle, char_name ? char_name : "unknown");
            // 对于我们定义的写特征值，通常不允许读
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }

        default:
            // 其他操作类型（如 Read Blob, Write Blob 等）我们暂时不处理
            ESP_LOGD(TAG, "Unhandled GATT operation: %d", ctxt->op);
            return BLE_ATT_ERR_UNLIKELY; // 返回通用错误
    }
}





// --- 其他函数暂时留空 ---
void BleConfig::gatt_svr_init(void) {
     int rc;
     rc = ble_gatts_count_cfg(gatt_svr_svcs);
     assert(rc == 0);
     rc = ble_gatts_add_svcs(gatt_svr_svcs);
     assert(rc == 0);
     ESP_LOGI(TAG, "GATT server initialized");
}

int BleConfig::gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char buf[BLE_UUID_STR_LEN];
     switch (ctxt->op) {
     case BLE_GATT_REGISTER_OP_SVC:
         ESP_LOGI(TAG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
         break;
     case BLE_GATT_REGISTER_OP_CHR:
         ESP_LOGI(TAG, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
         break;
     case BLE_GATT_REGISTER_OP_DSC:
         ESP_LOGI(TAG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
         break;
     default:
         assert(0);
         break;
     }
     return 0;
}

void BleConfig::ble_advertise(void) {
     struct ble_gap_adv_params adv_params;
     struct ble_hs_adv_fields fields;
     const char *name;
     int rc;

     memset(&fields, 0, sizeof fields);
     fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP; // 可发现，不支持经典蓝牙
     fields.tx_pwr_lvl_is_present = 1;
     fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

     name = ble_svc_gap_device_name(); // 获取设备名称
     fields.name = (uint8_t *)name;
     fields.name_len = strlen(name);
     fields.name_is_complete = 1;

     // --- 添加服务 UUID 到广播数据 ---
     ble_uuid128_t service_uuid;
     rc = ble_uuid_from_str(WIFI_CONFIG_SERVICE_UUID, &service_uuid);
     if (rc == 0) {
         fields.uuids128 = &service_uuid;
         fields.num_uuids128 = 1;
         fields.uuids128_is_complete = 1;
     } else {
         ESP_LOGE(TAG, "Failed to parse service UUID for advertising");
     }
     // --- 结束添加 ---

     rc = ble_gap_adv_set_fields(&fields);
     if (rc != 0) {
         ESP_LOGE(TAG, "error setting advertisement data; rc=%d\n", rc);
         return;
     }

     memset(&adv_params, 0, sizeof adv_params);
     adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // 可连接
     adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // 通用可发现模式

     rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, NULL, NULL); // 替换 ble_gap_event 回调为 NULL
     if (rc != 0) {
         ESP_LOGE(TAG, "error enabling advertisement; rc=%d\n", rc);
         return;
     }
     ESP_LOGI(TAG, "BLE advertising started");
}

void BleConfig::StartAdvertising() {
     ble_advertise();
}

void BleConfig::StopAdvertising() {
     ble_gap_adv_stop();
     ESP_LOGI(TAG, "BLE advertising stopped");
}

void BleConfig::ble_on_sync(void) {
     int rc;
     // 确保公共地址可用，或使用其他地址类型
     rc = ble_hs_util_ensure_addr(0);
     assert(rc == 0);
     // 开始广播
     ble_advertise();
}

void BleConfig::ble_on_reset(int reason) {
    ESP_LOGE(TAG, "Resetting state; reason=%d\n", reason);
}

void BleConfig::ble_host_task(void *param) {
     ESP_LOGI(TAG, "BLE Host Task Started");
     nimble_port_run(); // 此函数不会返回
     nimble_port_freertos_deinit();
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

    int rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error sending notification; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Sent Wi-Fi status notification: %d", status);
    }
}

void BleConfig::SetCredentialsReceivedCallback(std::function<void(const std::string& ssid, const std::string& password)> cb) {
    credentials_received_cb_ = cb;
}

void BleConfig::SetConnectWifiCallback(std::function<void()> cb) {
     connect_wifi_cb_ = cb;
 }

// --- NimBLE 事件处理 ---
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Connection %s; status=%d",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            // 记录连接句柄
             if(g_ble_config_instance) {
                 g_ble_config_instance->conn_handle_ = event->connect.conn_handle;
             }
             ESP_LOGI(TAG, "Connected, conn_handle=0x%x", event->connect.conn_handle);
             // 停止广播
             BleConfig::GetInstance().StopAdvertising();
        } else {
            // 连接失败，重新开始广播
             BleConfig::GetInstance().StartAdvertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnect; reason=%d", event->disconnect.reason);
         if(g_ble_config_instance) {
             g_ble_config_instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE; // 清除连接句柄
             g_ble_config_instance->received_ssid_.clear(); // 清除可能未处理完的凭据
             g_ble_config_instance->received_password_.clear();
         }
         // 重新开始广播，以便可以再次连接
         BleConfig::GetInstance().StartAdvertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
         // 可以选择在这里重新开始广播，如果需要的话
         // BleConfig::GetInstance().StartAdvertising();
        return 0;

     case BLE_GAP_EVENT_SUBSCRIBE:
         ESP_LOGI(TAG, "Subscribe event; conn_handle=%d attr_handle=%d "
                     "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                     event->subscribe.conn_handle,
                     event->subscribe.attr_handle,
                     event->subscribe.reason,
                     event->subscribe.prev_notify,
                     event->subscribe.cur_notify,
                     event->subscribe.prev_indicate,
                     event->subscribe.cur_indicate);
         return 0;

     case BLE_GAP_EVENT_MTU:
         ESP_LOGI(TAG, "MTU update event; conn_handle=%d cid=%d mtu=%d\n",
                     event->mtu.conn_handle,
                     event->mtu.channel_id,
                     event->mtu.value);
         return 0;
    }
    return 0;
}


// --- NimBLE Host 配置 ---
void ble_store_config_init(void); // Forward declaration

void BleConfig::ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");

    nimble_port_run(); // This function will return only when nimble_port_stop() is executed

    nimble_port_freertos_deinit();
}

void BleConfig::Initialize() {
    g_ble_config_instance = this; // 保存实例指针供静态回调使用
    ESP_LOGI(TAG, "Initializing BLE...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nimble_port_init();

    // 初始化蓝牙协议栈配置
     ble_hs_cfg.reset_cb = ble_on_reset;
     ble_hs_cfg.sync_cb = ble_on_sync;
     ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb; // 注册 GATT 回调
     ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 初始化GATT服务
     gatt_svr_init();

    // 设置设备名称
     rc = ble_svc_gap_device_name_set("xiaozhi-ble");
     assert(rc == 0);

     // 配置蓝牙安全管理器 (可选，但推荐)
     // ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT; // 根据你的安全需求配置
     // ble_hs_cfg.sm_bonding = 1;
     // ble_hs_cfg.sm_mitm = 0;
     // ble_hs_cfg.sm_sc = 1;

    // 配置存储回调 (如果需要持久化绑定信息)
     ble_store_config_init(); // If bonding is enabled

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE Initialized.");
}