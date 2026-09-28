/* ble_link.c — 见 ble_link.h
 *
 * 数据流：主机往 RX 特征写一条命令 → 拷进队列**立刻返回** → 自己的 worker
 * 任务取出来跑 lc_exec → 结果从 TX 特征 notify 回去。
 *
 * 为什么不在 GATT 回调里直接跑：`decision` 要阻塞几十秒等用户按键。GATT 回调
 * 跑在 NimBLE 的 host task 上，把它占住几十秒的话链路监督超时会直接把连接掐了。
 * 所以回调只做「拷贝 + 入队」，阻塞留给自己的 worker 任务。
 */
#include "ble_link.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "sdkconfig.h"

#if !CONFIG_TRAFFIC_ENABLE_BLE

/* QEMU 变体：没有蓝牙控制器，整块编掉，留个空实现免得调用方到处 #if */
void ble_link_start(const lc_host_t *h) { (void)h; ESP_LOGI("ble", "QEMU: 蓝牙不启用"); }
bool ble_link_notify(const char *txt) { (void)txt; return false; }
bool ble_link_connected(void) { return false; }

#else

static const char *TAG = "ble";

#define DEV_NAME     "TLMIAO"     /* 与 UDP 发现用的令牌同名，好认 */
#define CMD_MAX      160
#define PREF_MTU     256          /* 摘要文本是 UTF-8 中文，默认 23 字节装不下 */

/* 128 位 UUID，前缀 6d69616f = "miao"（ASCII）。
 * BLE_UUID128_INIT 收的是**小端**字节序，即把 UUID 字符串反过来写。
 * 注意宏体是 `.value = { uuid128 }`，所以这里**不能**再套一层花括号，
 * 套了就变成 `.value = {{...}}`——只有第一个字节生效，其余全丢。*/
#define UUID_SVC  0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, \
                  0x00, 0x40, 0x01, 0x00, 0x6f, 0x61, 0x69, 0x6d     /* …-0001-… */
#define UUID_RX   0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, \
                  0x00, 0x40, 0x02, 0x00, 0x6f, 0x61, 0x69, 0x6d     /* …-0002-… */
#define UUID_TX   0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, \
                  0x00, 0x40, 0x03, 0x00, 0x6f, 0x61, 0x69, 0x6d     /* …-0003-… */

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(UUID_SVC);
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(UUID_RX);
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(UUID_TX);

static const lc_host_t *s_host;
static QueueHandle_t    s_q;
static uint16_t         s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t         s_tx_val_handle;
static uint8_t          s_own_addr_type;

bool ble_link_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

/* ── GATT ────────────────────────────────────────────────────────────── */

/* RX：主机写命令进来。只做拷贝入队，绝不在这里执行。*/
static int gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    char buf[CMD_MAX];
    uint16_t om = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf - 1, &om);
    if (rc != 0) {
        ESP_LOGW(TAG, "读写入缓冲失败 rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[om] = '\0';
    if (om == 0) return 0;

    if (xQueueSend(s_q, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "命令队列满，丢弃: %s", buf);
    }
    return 0;
}

/* TX：只用来 notify，主机不会读它 */
static int gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg; (void)ctxt;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_tx_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ── 广播 / 连接 ─────────────────────────────────────────────────────── */

static void advertise(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "已连接 (handle %d)", s_conn);
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d，继续广播", event->connect.status);
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 reason=%d，重新广播", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "订阅 attr=%d notify=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;

    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEV_NAME;
    fields.name_len = (uint8_t)strlen(DEV_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv,
                           gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else       ESP_LOGI(TAG, "广播中，名字 \"%s\"", DEV_NAME);
}

/* ── worker：跑命令（可能阻塞几十秒）────────────────────────────────── */
static void ble_worker(void *arg) {
    (void)arg;
    char cmd[CMD_MAX], out[CMD_MAX];
    while (true) {
        if (xQueueReceive(s_q, cmd, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "命令: %s", cmd);
        if (!lc_exec(s_host, cmd, out, sizeof out))
            snprintf(out, sizeof out, "err unknown\n");
        ble_link_notify(out);
    }
}

bool ble_link_notify(const char *txt) {
    if (!txt) return false;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_tx_val_handle) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(txt, strlen(txt));
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify rc=%d", rc);
        return false;
    }
    return true;
}

/* ── 启动 ────────────────────────────────────────────────────────────── */

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "没有可用蓝牙地址");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "推断地址类型失败");
        return;
    }
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason=%d", reason); }

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();              /* 直到 nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_link_start(const lc_host_t *h) {
    s_host = h;
    s_q = xQueueCreate(4, CMD_MAX);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %d", err);
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* 不配对、不加密：和 HTTP 那条路一样是「谁都能发命令」的信任模型。
     * 想收紧就在这里开 SM + 静态密码，主机端也得跟着配对。*/
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

    ble_att_set_preferred_mtu(PREF_MTU);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败 rc=%d", rc);
        return;
    }
    ble_svc_gap_device_name_set(DEV_NAME);

    xTaskCreate(ble_worker, "ble_cmd", 4096, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);

    /* 把 UUID 打出来：这串必须和主机端（hooks/traffic-light.py 里的 BLE_SVC）
     * 完全一致。UUID 在结构体里是**小端**字节序，肉眼看不出来，打字符串最保险。*/
    char ubuf[40];
    if (ble_uuid_to_str(&s_svc_uuid.u, ubuf) == 0)
        ESP_LOGI(TAG, "服务 UUID: %s", ubuf);
    ESP_LOGI(TAG, "NimBLE 已启动，MTU 目标 %d", PREF_MTU);
}

#endif  /* CONFIG_TRAFFIC_ENABLE_BLE */
