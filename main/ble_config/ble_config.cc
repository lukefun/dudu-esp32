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
    ESP_LOGI(TAG, "开始解析UUID...");

    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_svc_wifi_config_uuid.u, WIFI_CONFIG_SERVICE_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_ssid_uuid.u, SSID_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_password_uuid.u, PASSWORD_CHAR_UUID);
    assert(rc == 0);
    rc = ble_uuid_from_str((ble_uuid_any_t*)&gatt_svr_chr_control_status_uuid.u, CONTROL_STATUS_CHAR_UUID);
    assert(rc == 0);
    
    ESP_LOGI(TAG, "UUID解析完成");
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
    ESP_LOGI(TAG, "开始初始化BLE配网模块...");

    // 1. 初始化NVS： "Non-Volatile Storage" 的缩写，即非易失性存储
    ESP_LOGI(TAG, "初始化NVS存储...");

    esp_err_t ret = nvs_flash_init();   // 初始化NVS

    // 检查NVS是否需要擦除(在使用非易失性存储（NVS）时没有可用的空闲页面。|| 新的NVS版本可用
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS需要擦除: %s", ret == ESP_ERR_NVS_NO_FREE_PAGES ? "无可用页面" : "新版本");
        ESP_ERROR_CHECK(nvs_flash_erase()); // 擦除NVS
        ret = nvs_flash_init(); // 再次初始化NVS
    }
    ESP_ERROR_CHECK(ret);   // 检查NVS初始化是否成功
    ESP_LOGI(TAG, "NVS初始化成功"); // 打印NVS初始化成功

    // 2. GATT服务配置
    ESP_LOGI(TAG, "配置GATT服务...");
    parse_all_uuids();
    
    // 3. Host初始化
    ESP_LOGI(TAG, "初始化NimBLE主机...");
    nimble_port_init();         // 初始化NimBLE主机
    ESP_LOGI(TAG, "NimBLE主机初始化完成");

    // 4. 主机配置
    ESP_LOGI(TAG, "初始化BLE存储配置...");
    ble_store_config_init();    // 初始化BLE存储
    ESP_LOGI(TAG, "BLE存储初始化成功");

    // === 安全配置开始 ===
    ESP_LOGI(TAG, "配置BLE安全参数...");
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; // 没有输入输出
    ble_hs_cfg.sm_bonding = 1;  // 启用配对
    ble_hs_cfg.sm_mitm = 1;     // 允许中间人攻击
    ble_hs_cfg.sm_sc = 1;       // 启用扫描
    ble_hs_cfg.sm_keypress = 0; // 不允许按键输入
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;  // 我们的密钥分发
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;    // 对方的密钥分发
    // === 安全配置结束 ===
    ESP_LOGI(TAG, "BLE安全参数配置完成");

    ESP_LOGI(TAG, "设置BLE回调函数...");
    ble_hs_cfg.reset_cb = ble_on_reset; // 重置回调
    ble_hs_cfg.sync_cb = ble_on_sync;   // 同步回调
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;    // GATT注册回调
    ble_hs_cfg.store_status_cb = NULL;  // 存储状态回调
    ESP_LOGI(TAG, "BLE回调函数设置完成");

    // 新版API注册方式（ESP-IDF v5.3.2+）
    // 3. 设备名称设置
    ESP_LOGI(TAG, "注册BLE事件监听器...");
    static struct ble_gap_event_listener ble_event_listener = {0};
    int rc = ble_gap_event_listener_register(
        &ble_event_listener,
        ble_gap_event,  // 注意：这里不需要取地址符&
        NULL);          // 没有需要传递的参数
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE事件监听器注册成功");
    } else {
        ESP_LOGE(TAG, "BLE事件监听器注册失败: %d", rc);
    }
    assert(rc == 0);

    ESP_LOGI(TAG, "初始化GATT服务器...");
    gatt_svr_init();

    ESP_LOGI(TAG, "设置BLE设备名称...");
    rc = ble_svc_gap_device_name_set("DuDu-BLE配网");
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE设备名称设置成功: DuDu-BLE配网");
    } else {
        ESP_LOGE(TAG, "BLE设备名称设置失败: %d", rc);
    }
    assert(rc == 0);

    // 4. Host任务启动
    ESP_LOGI(TAG, "启动NimBLE主机任务...");
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE初始化完成");
}

int BleConfig::gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char* char_name = (const char*)arg;
    int rc = 0;

    if (!g_ble_config_instance) {
        ESP_LOGE(TAG, "GATT访问失败: 全局实例不存在");
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
            ESP_LOGI(TAG, "收到特征值写入请求: %s, 数据长度: %d", char_name, data_len);

            // ==== 新增数据校验开始 ====
            // 控制命令必须为1字节
            if(strcmp(char_name, "control") == 0 && data_len != CONTROL_CMD_LEN) {
                ESP_LOGE(TAG, "控制命令长度无效: %d, 应为1字节", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // SSID最大32字节
            if(strcmp(char_name, "ssid") == 0 && data_len > MAX_SSID_LEN) {
                ESP_LOGE(TAG, "SSID长度过长: %d/32", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; 
            }
            // 密码最大64字节
            if(strcmp(char_name, "password") == 0 && data_len > MAX_PASSWORD_LEN) {
                ESP_LOGE(TAG, "密码长度过长: %d/64", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            // ==== 新增数据校验结束 ====

            if (data_len > 256) {   // 原有长度检查
                ESP_LOGE(TAG, "数据长度超过最大限制: %d/256", data_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            uint8_t* data = (uint8_t*)malloc(data_len + 1);
            if (!data) {
                ESP_LOGE(TAG, "内存分配失败，无法处理接收数据");
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, data_len, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "数据转换失败: %d", rc);
                free(data);
                return BLE_ATT_ERR_UNLIKELY;
            }
            data[data_len] = '\0';

            ESP_LOGI(TAG, "接收到 %d 字节数据: %.*s (十六进制: %s)", 
                    data_len, data_len, data, bytes_to_hex(data, data_len));

            if (char_name) {
                if (strcmp(char_name, "ssid") == 0) {
                    g_ble_config_instance->received_ssid_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "保存SSID: %s", g_ble_config_instance->received_ssid_.c_str());
                } else if (strcmp(char_name, "password") == 0) {
                    g_ble_config_instance->received_password_.assign((char*)data, data_len);
                    ESP_LOGI(TAG, "保存密码: %s", g_ble_config_instance->received_password_.c_str());
                } else if (strcmp(char_name, "control") == 0 && 
                          data_len == 1 && data[0] == WIFI_CONTROL_CMD_CONNECT) {
                    ESP_LOGI(TAG, "收到连接WiFi命令");
                    if (!g_ble_config_instance->received_ssid_.empty() && 
                        !g_ble_config_instance->received_password_.empty()) {
                        ESP_LOGI(TAG, "SSID和密码已接收，准备连接WiFi");
                        if (g_ble_config_instance->credentials_received_cb_) {
                            ESP_LOGI(TAG, "调用凭据接收回调");
                            g_ble_config_instance->credentials_received_cb_(
                                g_ble_config_instance->received_ssid_,
                                g_ble_config_instance->received_password_);
                        }
                        if (g_ble_config_instance->connect_wifi_cb_) {
                            ESP_LOGI(TAG, "调用WiFi连接回调");
                            g_ble_config_instance->connect_wifi_cb_();
                        }
                    } else {
                        ESP_LOGW(TAG, "收到连接命令但SSID或密码为空");
                    }
                }
            }
            free(data);
            break;
        }
        default:
            ESP_LOGW(TAG, "不支持的GATT操作: %d", ctxt->op);
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return rc;
}

void BleConfig::gatt_svr_init(void) {
    ESP_LOGI(TAG, "初始化GATT服务器...");
    int rc;
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT服务计数配置失败: %d", rc);
    }
    assert(rc == 0);
    
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "添加GATT服务失败: %d", rc);
    }
    assert(rc == 0);
    
    ESP_LOGI(TAG, "GATT服务器初始化成功");
}

void BleConfig::ble_advertise(void) {
    ESP_LOGI(TAG, "准备开始BLE广播...");
    struct ble_gap_adv_params adv_params;   // 广播参数
    struct ble_hs_adv_fields fields;        // 广播字段
    
    memset(&fields, 0, sizeof fields);      // 清空广播字段
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;    // 广播类型
    fields.tx_pwr_lvl_is_present = 1;                                   // 广播功率
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;                     // 广播功率 (自动)
    fields.name = (uint8_t *)ble_svc_gap_device_name();                 // 广播名称
    fields.name_len = strlen(ble_svc_gap_device_name());                // 广播名称长度
    fields.name_is_complete = 1;                                        // 广播名称是否完整
    fields.uuids128 = &gatt_svr_svc_wifi_config_uuid;                   // 广播UUID
    fields.num_uuids128 = 1;                                            // 广播UUID数量
    fields.uuids128_is_complete = 1;                                    // 广播UUID是否完整

    ESP_LOGI(TAG, "设置广播字段，设备名称: %s, 名称长度: %d", ble_svc_gap_device_name(), fields.name_len);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播字段失败: %d", rc);
        return;
    } else {
        ESP_LOGI(TAG, "广播字段设置成功");
    }

    // 增加广播功率和降低广播间隔以提高可发现性
    memset(&adv_params, 0, sizeof adv_params);                  // 清空广播参数
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL_MIN1 / 2;   // 降低间隔提高发现率
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL_MAX1 / 2;   // 降低间隔提高发现率
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;               // 连接模式
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;               // 发现模式

    ESP_LOGI(TAG, "开始广播，间隔: %d-%d (单位: 0.625ms)", adv_params.itvl_min, adv_params.itvl_max);

    // 开始广播
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                        &adv_params, ble_gap_event, NULL);

    if (rc == 0) {
        ESP_LOGI(TAG, "BLE广播已成功启动，设备名称: %s", ble_svc_gap_device_name());
    } else {
        ESP_LOGE(TAG, "启动BLE广播失败: %d", rc);
    }
}

void BleConfig::StartAdvertising() {
    ESP_LOGI(TAG, "尝试开始BLE广播...");
    if (ble_hs_synced()) {
        ESP_LOGI(TAG, "BLE主机已同步，立即开始广播");
        ble_advertise();
    } else {
        ESP_LOGW(TAG, "BLE主机尚未同步，将在同步后自动开始广播");
    }
}

void BleConfig::StopAdvertising() {
    ESP_LOGI(TAG, "尝试停止BLE广播...");
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            ESP_LOGI(TAG, "BLE广播已成功停止");
        } else {
            ESP_LOGE(TAG, "停止BLE广播失败: %d", rc);
        }
    } else {
        ESP_LOGI(TAG, "BLE广播已经处于停止状态");
    }
}

void BleConfig::ble_on_sync(void) {
    ESP_LOGI(TAG, "BLE主机同步完成，准备开始广播");

    // 获取设备地址
    uint8_t addr_val[6] = {0};
    int rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "设备MAC地址: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);
    } else {
        ESP_LOGE(TAG, "获取设备地址失败: %d", rc);
    }

    // 开始广播
    if (g_ble_config_instance) {
        ESP_LOGI(TAG, "BLE实例已初始化，开始广播");
        g_ble_config_instance->ble_advertise();
    } else {
        ESP_LOGE(TAG, "BLE实例未初始化，无法开始广播");
    }

    // int rc;
    // rc = ble_hs_util_ensure_addr(0);
    // assert(rc == 0);
    // ESP_LOGI(TAG, "BLE Host synced, starting advertising.");
    // ble_advertise();
}

void BleConfig::ble_on_reset(int reason) {
    ESP_LOGE(TAG, "BLE主机重置，原因: %d", reason);
}

void BleConfig::ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE主机任务已启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "BLE主机任务已停止");
}

void BleConfig::SendWifiStatus(wifi_config_status_t status) {
    ESP_LOGI(TAG, "尝试发送WiFi状态: %d", status);
    
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || status_val_handle_ == 0) {
        ESP_LOGW(TAG, "无法发送状态，没有连接或句柄无效 (conn_handle=%d, status_val_handle=%d)", 
                conn_handle_, status_val_handle_);
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&status, sizeof(status));
    if (!om) {
        ESP_LOGE(TAG, "为通知分配内存失败");
        return;
    }

    // === 修正重试机制：只在循环内发送，不再多发一次 ===
    int rc = -1;
    for(int retry = 0; retry < NOTIFY_RETRY_COUNT; retry++) {
        rc = ble_gatts_notify_custom(conn_handle_, status_val_handle_, om);
        if(rc == 0) {
            ESP_LOGI(TAG, "WiFi状态通知发送成功: %d", status);
            break;
        }
        ESP_LOGW(TAG, "通知发送失败 (尝试 %d/%d)，错误码: %d，稍后重试...", 
                retry+1, NOTIFY_RETRY_COUNT, rc);
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "发送通知失败，所有重试均失败; rc=%d", rc);
    }

    os_mbuf_free_chain(om);  // 确保释放mbuf
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
        ESP_LOGE(TAG, "BLE事件处理失败：全局实例不存在");
        return 0;
    }

    ESP_LOGD(TAG, "收到BLE事件: %d", event->type);
    
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
        ESP_LOGI(TAG, "BLE连接事件 - 状态: %d", event->connect.status);
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE设备已连接，连接句柄: %d", event->connect.conn_handle);
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "连接设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                        desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], 
                        desc.peer_id_addr.val[3], desc.peer_id_addr.val[2], 
                        desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            } else {
                ESP_LOGW(TAG, "无法获取连接设备信息: %d", rc);
            }
            g_ble_config_instance->conn_handle_ = event->connect.conn_handle;
            ESP_LOGI(TAG, "保存连接句柄: %d", event->connect.conn_handle);
            g_ble_config_instance->StopAdvertising();
        } else {
            ESP_LOGW(TAG, "连接失败，重新开始广播");
            g_ble_config_instance->StartAdvertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE断开连接 - 原因: %d", event->disconnect.reason);
        g_ble_config_instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "连接已断开，重新开始广播");
        g_ble_config_instance->StartAdvertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE广播完成事件 - 状态: %d", event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE订阅事件 - 连接句柄: %d, 属性句柄: %d, 订阅状态: %d", 
                event->subscribe.conn_handle, event->subscribe.attr_handle, 
                event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == g_ble_config_instance->status_val_handle_ &&
            event->subscribe.cur_notify) {
            ESP_LOGI(TAG, "客户端已订阅状态通知，发送初始状态");
            g_ble_config_instance->SendWifiStatus(WIFI_STATUS_IDLE);
        }
        return 0;

    default:
        ESP_LOGD(TAG, "未处理的BLE事件: %d", event->type);
        return 0;
    }
}