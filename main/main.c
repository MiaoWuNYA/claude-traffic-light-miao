/* traffic-light-miao — Claude 状态红绿灯 for 小喵掌机
 *
 * 移植自 github.com/safernandez666/vibecoding-traffic-light
 * 硬件框架来自 github.com/jsfaint/tetris-miao (ESP32-WROVER-B + ST7735 160x128)
 *
 * Claude hooks (POST):
 *   /solo/rojo   — UserPromptSubmit: Claude 开始工作 → 红屏
 *   /alerta      — Notification: 需要确认     → 黄屏闪烁 (30s)
 *   /solo/verde  — Stop: 完成               → 绿屏
 *   /off         — 熄屏
 *
 * 首次开机 / 按 UP 进入 WiFi 设置：扫描热点 → 选 SSID → 屏幕键盘输密码 → 连接。
 * 凭据存 NVS，下次开机自动连接。另在 UDP 4210 应答 "TLMIAO?"，供主机脚本自动找 IP。
 */
#include <ctype.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_TRAFFIC_ENABLE_WIFI
#include "lwip/sockets.h"
#endif

#include "return_to_loader.h"
#include "mochi.h"
#include "beep.h"
#include "ble_link.h"
#include "link_cmd.h"

LV_FONT_DECLARE(lv_font_sc14);   /* Noto Sans SC 子集（含中文 + ASCII） */
LV_FONT_DECLARE(lv_font_sc12);

/* ── Hardware Constants（与 tetris-miao 相同）──────────────────────────── */
#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160
#define LCD_H_RES           160
#define LCD_V_RES           128
#define LVGL_TICK_PERIOD_MS 1

#define PIN_LCD_SCLK   GPIO_NUM_18
#define PIN_LCD_MOSI   GPIO_NUM_23
#define PIN_LCD_RES    GPIO_NUM_19   /* 复位脚（与 SD 卡 MISO 共享；SPI 只写不用 MISO）*/
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_DC     GPIO_NUM_4

/* ── 按键（与 tetris-miao 相同，低电平有效）────────────────────────────── */
#if CONFIG_TRAFFIC_ENABLE_WIFI
static const gpio_num_t s_btn[6] = {
    GPIO_NUM_2,  GPIO_NUM_13, GPIO_NUM_27,
    GPIO_NUM_35, GPIO_NUM_34, GPIO_NUM_12,
};  /* 上 下 左 右 A B */
#endif

/* ── UI 配色 (RGB888) ─────────────────────────────────────────────────── */
#define UI_BG       0x1B1713
#define UI_TEXT     0xFFF3B0
#define UI_DIM      0x9C8A6A
#define UI_SEL      0x4A3A1E
#define C_RED       0xE5484D
#define C_YELLOW    0xF6D34A
#define C_GREEN     0x3BD64F
#define C_OFF       0x2A2318

#define ALERTA_MS   30000   /* 黄灯闪烁总时长 */
#define BLINK_MS    350     /* 闪烁周期半拍 */

#define MAX_AP      16      /* 最多保存的热点数 */
#define LIST_ROWS   6       /* 扫描列表可见行数 */

static const char *TAG = "traffic";

/* ── 状态机 ───────────────────────────────────────────────────────────── */
typedef enum { ST_OFF, ST_RED, ST_YELLOW, ST_GREEN } light_t;

/* ── 会话槽：支持多个 Claude 多开 ───────────────────────────────────────
 * 每个电脑侧 agent 实例占一个槽（钩子用 session_id 映射，HTTP ?s=N / BLE @N）。
 * 槽 0 是缺省槽，不带参数的旧命令全落这里。灯只有一个，显示聚合态：
 * 黄 > 红 > 绿 > 灭（最需要人管的赢）。 */
#define SESS_MAX 4
typedef struct {
    light_t  st;
    uint32_t run_start;         /* 计时起点（st==ST_RED 时有效） */
    uint32_t run_ms;            /* 冻结耗时（绿灯后） */
    bool     run_live;
    bool     alert_fin;         /* 黄灯 30s 到点了吗（按槽记） */
    char     task[40];
    char     question[48];      /* 授权问句：要授权的是什么操作 */
} sess_t;
static sess_t s_sessions[SESS_MAX];

static bool          s_alert_on   = false;
static uint32_t      s_alert_start = 0;   /* 聚合态最近一次变黄的时刻 */
static uint32_t      s_blink_next = 0;
static bool          s_blink_fase = false;
static volatile bool s_wifi_ok    = false;

static char s_ipbuf[16] = "";

/* ── LCD（与 tetris-miao 相同）─────────────────────────────────────────── */
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_DISPOFF  0x28
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1
#define MADCTL_MX 0x40
#define MADCTL_MY 0x80
#define MADCTL_MV 0x20
#define MADCTL_RGB 0x00

#if CONFIG_TRAFFIC_ENABLE_WIFI
static void st_tx(esp_lcd_panel_io_handle_t io, int cmd,
                  const void *param, size_t len) {
    esp_lcd_panel_io_tx_param(io, cmd, param, len);
}
static void st_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st7735_init(esp_lcd_panel_io_handle_t io) {
    const uint8_t frmctr[]  = {0x01,0x2C,0x2D};
    const uint8_t frmctr3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
    const uint8_t pwctr1[]  = {0xA2,0x02,0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A,0x00};
    const uint8_t pwctr4[]  = {0x8A,0x2A};
    const uint8_t pwctr5[]  = {0x8A,0xEE};
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};
    const uint8_t colmod[] = {0x05};
    const uint8_t gp[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                          0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gn[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                          0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};

    st_tx(io, ST7735_DISPOFF, NULL, 0);
    st_tx(io, ST7735_SWRESET, NULL, 0);
    st_delay(150);
    st_tx(io, ST7735_SLPOUT, NULL, 0);
    st_delay(500);
    st_tx(io, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st_tx(io, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st_tx(io, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st_tx(io, ST7735_INVCTR, (uint8_t[]){0x07}, 1);
    st_tx(io, ST7735_PWCTR1, pwctr1, sizeof(pwctr1));
    st_tx(io, ST7735_PWCTR2, pwctr2, sizeof(pwctr2));
    st_tx(io, ST7735_PWCTR3, pwctr3, sizeof(pwctr3));
    st_tx(io, ST7735_PWCTR4, pwctr4, sizeof(pwctr4));
    st_tx(io, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st_tx(io, ST7735_VMCTR1, (uint8_t[]){0x0E}, 1);
    st_tx(io, ST7735_INVOFF, NULL, 0);
    st_tx(io, ST7735_MADCTL, madctl_d, sizeof(madctl_d));
    st_tx(io, ST7735_COLMOD, colmod, sizeof(colmod));
    st_tx(io, ST7735_CASET, (uint8_t[]){0,0,0,LCD_NATIVE_H_RES-1}, 4);
    st_tx(io, ST7735_RASET, (uint8_t[]){0,0,0,LCD_NATIVE_V_RES-1}, 4);
    st_tx(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st_tx(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st_tx(io, ST7735_NORON, NULL, 0);
    st_delay(10);
    st_tx(io, ST7735_MADCTL, madctl_r, sizeof(madctl_r));
}
#endif  /* CONFIG_TRAFFIC_ENABLE_WIFI */

/* SPI 实际跑多快（被 SPI 驱动 clamp 之后就查不出来了，自己记一份给 /status 看）*/
static int s_spi_hz = LCD_PIXEL_CLOCK_HZ;

static esp_lcd_panel_io_handle_t lcd_init(void) {
#if !CONFIG_TRAFFIC_ENABLE_WIFI
    /* QEMU 的 esp32 SPI 模型不会产生传输完成中断，tx_param/tx_color 会永久阻塞，
     * 所以无 WiFi 构建整条 SPI 链路都不走，只验证 LVGL 渲染与业务逻辑 */
    ESP_LOGI(TAG, "qemu build: LCD/SPI skipped");
    return NULL;
#else
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    /* 面板复位：GPIO19 拉低 ≥10ms 再拉高，不复位的话面板保持白屏 */
    gpio_config_t rc = {
        .pin_bit_mask = 1ULL << PIN_LCD_RES, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rc);
    gpio_set_level(PIN_LCD_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t cfg = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &io));
    st7735_init(io);

    ESP_LOGI(TAG, "st7735 ready @ %d MHz", s_spi_hz / 1000000);
    return io;
#endif
}

/* ── LVGL 显示 ────────────────────────────────────────────────────────── */
#if !CONFIG_TRAFFIC_ENABLE_WIFI
static volatile bool s_dump_req = false;

/* 把活动屏幕上的 label 文字和坐标打出来，验证内容 */
static void dump_obj(lv_obj_t *o, int depth) {
    if (lv_obj_check_type(o, &lv_label_class)) {
        lv_obj_t *p = lv_obj_get_parent(o);
        printf("%*sL(%3d,%3d) \"%s\"%s\n", depth * 2, "",
               (int)(lv_obj_get_x(p) + lv_obj_get_x(o)),
               (int)(lv_obj_get_y(p) + lv_obj_get_y(o)),
               lv_label_get_text(o),
               lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? " [hidden]" : "");
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) dump_obj(lv_obj_get_child(o, i), depth + 1);
}

/* QEMU 无屏幕：把帧缓冲降采样成字符画打到串口，用来肉眼验证 UI 版式 */
static void dump_frame(const uint16_t *px) {
    dump_obj(lv_screen_active(), 0);
    for (int y = 0; y < LCD_V_RES; y += 4) {
        char line[LCD_H_RES / 2 + 1];
        for (int x = 0; x < LCD_H_RES; x += 2) {
            uint16_t v = px[y * LCD_H_RES + x];
            v = (uint16_t)((v >> 8) | (v << 8));   /* RGB565_SWAPPED → 正常 */
            int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            int lum = (r * 2 + g * 4 + b) / 7;     /* 0..63 */
            line[x / 2] = lum <= 5 ? ' ' : lum <= 10 ? '.' : lum <= 30 ? '+' : '#';
        }
        line[LCD_H_RES / 2] = 0;
        printf("%s\n", line);
    }
    printf("\n");
    fflush(stdout);
}
#endif

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px) {
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(d);
#if CONFIG_TRAFFIC_ENABLE_WIFI
    uint16_t x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    esp_err_t e1 = esp_lcd_panel_io_tx_param(io, ST7735_CASET,
        (uint8_t[]){x1>>8,x1&0xFF,x2>>8,x2&0xFF}, 4);
    esp_err_t e2 = esp_lcd_panel_io_tx_param(io, ST7735_RASET,
        (uint8_t[]){y1>>8,y1&0xFF,y2>>8,y2&0xFF}, 4);
    int sz = (x2-x1+1)*(y2-y1+1)*2;
    /* tx_color 是异步 DMA（esp_lcd 队列模式）：只入队不等待。
     * 不能在这里立即 flush_ready——DMA 还在搬这块缓冲时 LVGL 就开始画
     * 下一帧会导致花屏（mochi 表情 90ms 全屏重绘后必现）。
     * 真完成由 on_color_done 回调标记。*/
    esp_err_t e3 = esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, sz);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        /* 出错时 DMA 完成回调不会来了，再不放行 LVGL 就会死等 —— 死等发生在
         * app_main 里（lv_timer_handler），IDLE0 饿死，几十秒后 task_wdt 报警
         * 并打进重启循环。放行的话最坏只是这一帧不显示。 */
        ESP_LOGE(TAG, "flush 失败: %d/%d/%d，本帧跳过", e1, e2, e3);
        lv_display_flush_ready(d);
    }
#else
    (void)io; (void)area;
    if (s_dump_req) { s_dump_req = false; dump_frame((const uint16_t *)px); }
    /* QEMU 无真实 DMA，立即标记完成（esp_lcd 的 trans_done 中断在 QEMU 下不可靠） */
    lv_display_flush_ready(d);
#endif
}

#if CONFIG_TRAFFIC_ENABLE_WIFI
/* esp_lcd 颜色传输真正完成（DMA 搬完）时在 SPI 任务上下文回调 */
static bool on_color_done(esp_lcd_panel_io_handle_t panel_io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_display_t *d = user_ctx;
    lv_display_flush_ready(d);
    return false;
}
#endif

static void tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static lv_display_t *display_init(esp_lcd_panel_io_handle_t io) {
    lv_display_t *d = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, cf);
    size_t sz = stride * LCD_V_RES;
#if CONFIG_TRAFFIC_ENABLE_WIFI
    /* 帧缓冲只能放内部 RAM 的 DMA 区（ESP32 的 DMA 读不了 PSRAM），但加了
     * NimBLE 之后蓝牙的静态段把 DRAM 堆切成碎片，连一块连续 40KB 都没了
     * （boot_mem_report 的日志能对上）。所以放弃整屏双缓冲，改 partial 模式：
     * LVGL 每次 render/flush FB_LINES 行，小块 DMA 缓冲必然放得下。
     * 代价是全屏重绘要分几趟传输，mochi 动画的帧率略降——可接受。 */
#define FB_LINES 20
    /* 颜色传输完成回调 → flush_ready；display 作 user_ctx 传给回调 */
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, d));
    sz = stride * FB_LINES;
    void *b1 = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    void *b2 = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    assert(b1 && b2);
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    void *b1 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    void *b2 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    assert(b1 && b2);
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_FULL);
#endif
    lv_display_set_user_data(d, io);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

/* ── 按键 → 事件 ──────────────────────────────────────────────────────── */
enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B };
#define EV_UP    1
#define EV_DOWN  2
#define EV_LEFT  3
#define EV_RIGHT 4
#define EV_A     5
#define EV_B     6
#define EV_LONG  0x40
#define LONG_MS  600
#define DEBOUNCE_MS 25

#define EVQ_LEN 12
static uint8_t s_evq[EVQ_LEN];
static int     s_evq_h, s_evq_t;

static void ev_push(uint8_t e) {
    int n = (s_evq_t + 1) % EVQ_LEN;
    if (n == s_evq_h) return;          /* 满则丢弃 */
    s_evq[s_evq_t] = e;
    s_evq_t = n;
}
static bool ev_pop(uint8_t *e) {
    if (s_evq_h == s_evq_t) return false;
    *e = s_evq[s_evq_h];
    s_evq_h = (s_evq_h + 1) % EVQ_LEN;
    return true;
}

static void input_init(void) {
#if CONFIG_TRAFFIC_ENABLE_WIFI
    uint64_t mask = 0, pullup = 0;
    for (int i = 0; i < 6; i++) {
        mask |= 1ULL << s_btn[i];
        if (s_btn[i] != GPIO_NUM_34 && s_btn[i] != GPIO_NUM_35)
            pullup |= 1ULL << s_btn[i];
    }
    gpio_config_t io = {
        .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    if (pullup) {
        gpio_config_t pu = {
            .pin_bit_mask = pullup, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pu);
    }
#else
    /* QEMU：串口 RX FIFO 轮询当按键（不装驱动，避免中断在 QEMU 下不触发）*/
    uart_ll_rxfifo_rst(UART_LL_GET_HW(0));
    ESP_LOGI(TAG, "uart key input ready (poll RX fifo)");
#endif
}

static void input_scan(void) {
#if CONFIG_TRAFFIC_ENABLE_WIFI
    static uint8_t  raw_last[6] = {0};
    static uint8_t  stable[6]   = {0};
    static uint32_t chg[6];
    static uint32_t down_tick[6];
    static uint8_t  long_done = 0;
    static uint32_t key_held = 0;        /* 当前按住的键位掩码（去抖后） */
    uint32_t now = lv_tick_get();

    for (int i = 0; i < 6; i++) {
        uint8_t r = gpio_get_level(s_btn[i]) == 0 ? 1 : 0;
        if (r != raw_last[i]) { raw_last[i] = r; chg[i] = now; }
        if (lv_tick_elaps(chg[i]) >= DEBOUNCE_MS) stable[i] = raw_last[i];
        bool was = (key_held >> i) & 1;
        if (stable[i] && !was) {
            key_held |= 1u << i;
            down_tick[i] = now;
        } else if (!stable[i] && was) {
            key_held &= ~(1u << i);
            if (!(long_done & (1u << i))) ev_push(i + 1);
            long_done &= ~(1u << i);
        }
        if ((key_held & (1u << i)) && !(long_done & (1u << i)) &&
            lv_tick_elaps(down_tick[i]) >= LONG_MS) {
            long_done |= 1u << i;
            ev_push((i + 1) | EV_LONG);
        }
    }
#else
    uart_dev_t *u = UART_LL_GET_HW(0);
    uint32_t n = uart_ll_get_rxfifo_len(u);
    if (n) {
        uint8_t buf[128];
        if (n > sizeof buf) n = sizeof buf;
        uart_ll_read_rxfifo(u, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = buf[i], e = 0;
            switch (c) {
            case 'u': e = EV_UP;    break;
            case 'd': e = EV_DOWN;  break;
            case 'l': e = EV_LEFT;  break;
            case 'r': e = EV_RIGHT; break;
            case 'a': e = EV_A;     break;
            case 'b': e = EV_B;     break;
            case 'U': e = EV_UP    | EV_LONG; break;
            case 'D': e = EV_DOWN  | EV_LONG; break;
            case 'L': e = EV_LEFT  | EV_LONG; break;
            case 'R': e = EV_RIGHT | EV_LONG; break;
            case 'A': e = EV_A     | EV_LONG; break;
            case 'B': e = EV_B     | EV_LONG; break;
            case 'p': s_dump_req = true; ESP_LOGI(TAG, "key: dump"); break;
            default: break;
            }
            if (e) { ev_push(e); ESP_LOGI(TAG, "key: %c", c); }
        }
    }
#endif
}

/* ── WiFi 凭据（NVS）──────────────────────────────────────────────────── */
#define CFG_NS "tlcfg"

static bool cfg_load(char *ssid, size_t ss, char *pass, size_t ps) {
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l1 = ss, l2 = ps;
    bool ok = nvs_get_str(h, "ssid", ssid, &l1) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, "pass", pass, &l2) != ESP_OK) pass[0] = 0;
    nvs_close(h);
    return ok;
}

static void cfg_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ── WiFi 后台任务 ────────────────────────────────────────────────────── */
typedef enum { WCMD_SCAN, WCMD_CONNECT } wcmd_t;
typedef enum { WRES_SCAN, WRES_CONN } wres_t_kind;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} ap_t;

typedef struct { wcmd_t cmd; char ssid[33]; char pass[64]; } wreq_t;
typedef struct { int kind; int ok; int nap; ap_t ap[MAX_AP]; } wres_t;

static QueueHandle_t s_wreq, s_wres;
static volatile bool s_auto_reconnect = false;

#if CONFIG_TRAFFIC_ENABLE_WIFI
static EventGroupHandle_t s_wev;
#define WEV_GOTIP BIT0
#define WEV_FAIL  BIT1

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        if (s_auto_reconnect) {
            esp_wifi_connect();
        } else if (s_wev) {
            xEventGroupSetBits(s_wev, WEV_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ipbuf, sizeof s_ipbuf, IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_ok = true;
        if (s_wev) xEventGroupSetBits(s_wev, WEV_GOTIP);
        ESP_LOGI(TAG, "got ip: %s", s_ipbuf);
    }
}

static void wifi_worker(void *arg) {
    wreq_t q;
    while (xQueueReceive(s_wreq, &q, portMAX_DELAY)) {
        wres_t r = {0};
        if (q.cmd == WCMD_SCAN) {
            r.kind = WRES_SCAN;
            esp_wifi_scan_start(NULL, true);          /* 阻塞扫描 */
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            if (n > 40) n = 40;
            wifi_ap_record_t *rec = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
            if (rec && n) esp_wifi_scan_get_ap_records(&n, rec);
            for (int i = 0; rec && i < n && r.nap < MAX_AP; i++) {
                if (!rec[i].ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < r.nap; j++)
                    if (!strcmp(r.ap[j].ssid, (char *)rec[i].ssid)) { dup = true; break; }
                if (dup) continue;
                int k = r.nap;                        /* 按信号强度插入排序 */
                while (k > 0 && r.ap[k-1].rssi < rec[i].rssi) { r.ap[k] = r.ap[k-1]; k--; }
                strlcpy(r.ap[k].ssid, (char *)rec[i].ssid, sizeof r.ap[k].ssid);
                r.ap[k].rssi = rec[i].rssi;
                r.ap[k].auth = rec[i].authmode;
                r.nap++;
            }
            free(rec);
            ESP_LOGI(TAG, "scan done: %d ap", r.nap);
        } else {
            r.kind = WRES_CONN;
            wifi_config_t wc = {0};
            strlcpy((char *)wc.sta.ssid, q.ssid, sizeof wc.sta.ssid);
            strlcpy((char *)wc.sta.password, q.pass, sizeof wc.sta.password);
            s_auto_reconnect = false;
            xEventGroupClearBits(s_wev, WEV_GOTIP | WEV_FAIL);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            esp_wifi_connect();
            EventBits_t b = xEventGroupWaitBits(s_wev, WEV_GOTIP | WEV_FAIL,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
            r.ok = (b & WEV_GOTIP) ? 1 : 0;
            s_auto_reconnect = r.ok;
            ESP_LOGI(TAG, "connect %s: %s", q.ssid, r.ok ? "ok" : "fail");
        }
        xQueueSend(s_wres, &r, portMAX_DELAY);
    }
}

/* UDP 应答：主机端 hook 脚本广播 "TLMIAO?" 找设备 IP */
static void udp_task(void *arg) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(4210);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGW(TAG, "udp bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    char buf[80];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    while (1) {
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "TLMIAO?", 7) == 0 && s_wifi_ok) {
            char rep[48];
            int m = snprintf(rep, sizeof rep, "TLMIAO %s", s_ipbuf);
            sendto(s, rep, m, 0, (struct sockaddr *)&from, fl);
        }
    }
}

static void wifi_start(void) {
    s_wev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 关 modem sleep：默认省电模式下射频周期性休眠，钩子的短超时请求
     * 经常正好撞上设备睡着的间隙而超时——「命令经常不生效」的主因。
     * 掌机常插着电，省这点电不值。 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    xTaskCreate(udp_task, "udp", 3072, NULL, 4, NULL);
}
#else /* !CONFIG_TRAFFIC_ENABLE_WIFI —— QEMU 假实现，用来验证 UI 流程 */
static void wifi_worker(void *arg) {
    wreq_t q;
    while (xQueueReceive(s_wreq, &q, portMAX_DELAY)) {
        wres_t r = {0};
        if (q.cmd == WCMD_SCAN) {
            vTaskDelay(pdMS_TO_TICKS(1200));
            r.kind = WRES_SCAN;
            r.nap = 4;
            strlcpy(r.ap[0].ssid, "MyPhone-Hotspot", sizeof r.ap[0].ssid);
            r.ap[0].rssi = -45; r.ap[0].auth = WIFI_AUTH_WPA2_PSK;
            strlcpy(r.ap[1].ssid, "Xiaomi_2.4G", sizeof r.ap[1].ssid);
            r.ap[1].rssi = -63; r.ap[1].auth = WIFI_AUTH_WPA_WPA2_PSK;
            strlcpy(r.ap[2].ssid, "FreeWiFi", sizeof r.ap[2].ssid);
            r.ap[2].rssi = -78; r.ap[2].auth = WIFI_AUTH_OPEN;
            strlcpy(r.ap[3].ssid, "CMCC-Home", sizeof r.ap[3].ssid);
            r.ap[3].rssi = -88; r.ap[3].auth = WIFI_AUTH_WPA2_PSK;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1500));
            r.kind = WRES_CONN;
            r.ok = (q.pass[0] || !strcmp(q.ssid, "FreeWiFi")) ? 1 : 0;
            if (!strcmp(q.ssid, "Xiaomi_2.4G")) r.ok = 0;   /* 假装密码错，测失败路径 */
            if (r.ok) strlcpy(s_ipbuf, "192.168.43.99", sizeof s_ipbuf);
        }
        xQueueSend(s_wres, &r, portMAX_DELAY);
    }
}
static void wifi_start(void) {
    snprintf(s_ipbuf, sizeof s_ipbuf, "0.0.0.0");
}
#endif

static void wifi_req_scan(void) {
    wreq_t q = { .cmd = WCMD_SCAN };
    xQueueSend(s_wreq, &q, 0);
}
static void wifi_req_connect(const char *ssid, const char *pass) {
    wreq_t q = { .cmd = WCMD_CONNECT };
    strlcpy(q.ssid, ssid, sizeof q.ssid);
    strlcpy(q.pass, pass, sizeof q.pass);
    xQueueSend(s_wreq, &q, 0);
}

/* ── UI ───────────────────────────────────────────────────────────────── */
/* 屏幕布局（160x128）：
 *   主界面   三个圆形灯 + 状态文字 + IP
 *   热点列表 标题 + 6 行 SSID（含"返回"项）
 *   密码输入 SSID + 已输密码 + 6x10 字符网格（首行是 大小写/删除/确认）*/
static lv_obj_t *s_scr_home, *s_scr_wifi, *s_scr_pass, *s_scr_ask, *s_scr_menu;
static lv_obj_t *s_lbl_ask_q;          /* ask 界面的问句行 */

/* 主界面（mochi 表情脸 + 状态文字 + 底部一行摘要/IP）*/
static lv_obj_t *s_lbl_state, *s_lbl_foot, *s_lbl_time;

/* 热点列表 */
static lv_obj_t *s_wifi_msg;
static lv_obj_t *s_row[LIST_ROWS], *s_row_name[LIST_ROWS], *s_row_info[LIST_ROWS];

/* 密码输入 */
static lv_obj_t *s_pw_ssid, *s_pw_text, *s_pw_msg;
static lv_obj_t *s_cell[6][10], *s_cell_lbl[6][10];
#define CELL_H 15
#define CELL_Y0 33

static char s_sel_ssid[33];
static char s_pw[64];
static int  s_pwlen;
static ap_t s_ap[MAX_AP];
static int  s_nap, s_cursor, s_scroll;
static int  s_grid_row = 1, s_grid_col;
static bool s_shift;
static bool s_scanning, s_connecting;
static char s_msg[24];
static uint32_t s_msg_until;

typedef enum { SCR_HOME, SCR_WIFI, SCR_PASS, SCR_ASK, SCR_MENU } scr_t;
static scr_t s_scr = SCR_HOME;

/* ── 底部一行：任务摘要 ↔ IP ──────────────────────────────────────────── */
static bool s_foot_show_ip = false;     /* 菜单里「显示 IP」切换 */

/* ── 链路模式：WiFi(HTTP) 还是蓝牙(BLE) ─────────────────────────────────
 * 存在 NVS，开机时决定起哪一套。切换需要重启（菜单里选完就重启）。*/
static bool s_ble_mode = false;

/* ── 授权（按会话槽隔离）────────────────────────────────────────────────
 * 电脑侧两段式：/decision/start 登记 pending → /decision/poll 轮询结果。
 * poll 是毫秒级请求，httpd 不再被 60 秒长轮询占死——这是「多开时别的
 * Claude 推送不生效」的修复之一。BLE 的 decision 命令（阻塞式）跑在
 * ble worker 自己的任务里，仍然走 decide_wait。
 * 按键后主循环写 dec——主循环是唯一允许碰 LVGL 的一方。 */
#define DEC_NONE   0
#define DEC_ALLOW  1    /* ↑ 继续：这一次允许 */
#define DEC_DENY   2    /* → 拒绝 */
#define DEC_ALWAYS 3    /* ← 总是接受更改：允许并写白名单 */
#define DEC_RETRY  4    /* ↓ 重来：拒绝并附言让 Claude 重试 */
typedef struct {
    volatile int  dec;          /* DEC_NONE = 还没答 */
    volatile bool pending;      /* 有电脑侧在等这个槽的决定 */
    volatile uint32_t deadline; /* start 时的 timeout（ms tick），主循环用来清账 */
} ask_t;
static ask_t s_asks[SESS_MAX];
static volatile int s_ask_slot = -1;   /* ask 界面正在回答哪个槽（主循环维护） */

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt,
                          const lv_font_t *f, uint32_t col) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

/* 按「半角 1 单位、全角 2 单位」截断文本到 max_units 单位，超出补 ".."。
 * LVGL 的 label 不会自己裁掉溢出的字，会直接画到屏幕外，所以凡是要上屏的
 * 动态文本都先过这里。 */
static void fit_text(char *out, size_t osz, const char *txt, int max_units) {
    size_t o = 0;
    int units = 0;
    const unsigned char *p = (const unsigned char *)(txt ? txt : "");
    while (*p && units < max_units) {
        int len = (*p < 0x80) ? 1 : (*p < 0xE0) ? 2 : (*p < 0xF0) ? 3 : 4;
        int adv = (*p < 0x80) ? 1 : 2;          /* 半角 1 单位，全角 2 单位 */
        if (units + adv > max_units) break;
        if (o + (size_t)len + 3 > osz) break;
        for (int i = 0; i < len && p[i]; i++) out[o++] = (char)p[i];
        p += len;
        units += adv;
    }
    if (*p) { out[o++] = '.'; out[o++] = '.'; }
    out[o] = '\0';
}

static void foot_set(const char *txt) {
    char out[80];
    fit_text(out, sizeof out, txt, 24);
    lv_label_set_text(s_lbl_foot, out);
}

static void screen_style(lv_obj_t *s) {
    lv_obj_set_style_bg_color(s, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
}

static const char *state_text(light_t st) {
    switch (st) {
    case ST_RED:    return "CLAUDE BUSY";
    case ST_YELLOW: return "NEEDS YOU !";
    case ST_GREEN:  return "TASK DONE";
    default:        return "OFF";
    }
}

static mochi_state_t mochi_of(light_t st) {
    switch (st) {
    case ST_RED:    return MO_RED;
    case ST_YELLOW: return MO_YELLOW;
    case ST_GREEN:  return MO_GREEN;
    default:        return MO_OFF;
    }
}

static void build_home(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    screen_style(scr);
    s_scr_home = scr;

    /* cc-mochi 表情脸占全屏（画在背景层），label 叠在上面 */
    mochi_build(scr);

    /* 状态字挪到左上：右上角要留给计时器，两者都居中的话「CLAUDE BUSY」
     * 会正好压到计时器上（160px 宽，14px 字已经占 ~108px）。*/
    s_lbl_state = mk_label(scr, "BOOT...", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_align(s_lbl_state, LV_ALIGN_TOP_LEFT, 4, 5);

    /* 计时器：和 Claude Code 终端里那个秒数同源（都从「提交输入」起算）。
     * 用 montserrat_10，10px 的 "1m23s" 约 30px，塞得进右上角。*/
    s_lbl_time = mk_label(scr, "", &lv_font_montserrat_10, UI_DIM);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_RIGHT, -4, 6);

    s_lbl_foot = mk_label(scr, "", &lv_font_sc12, UI_TEXT);
    lv_obj_align(s_lbl_foot, LV_ALIGN_BOTTOM_MID, 0, -3);
    foot_set("");
}

/* 计时文本，格式对齐 Claude Code 终端的写法：不到一分钟只显示秒，
 * 之后是 1m23s，超过一小时 1h02m。全 ASCII，字体一定覆盖得到。
 * 显示的是聚合优先槽（正在黄/红的那一路）的耗时。 */
static void run_text(char *out, size_t n, int slot) {
    const sess_t *s = &s_sessions[slot];
    uint32_t ms = s->run_live ? (lv_tick_get() - s->run_start) : s->run_ms;
    uint32_t sec = ms / 1000;
    if (sec < 60)          snprintf(out, n, "%us", (unsigned)sec);
    else if (sec < 3600)   snprintf(out, n, "%um%02us", (unsigned)(sec / 60),
                                    (unsigned)(sec % 60));
    else                   snprintf(out, n, "%uh%02um", (unsigned)(sec / 3600),
                                    (unsigned)((sec / 60) % 60));
}

static void build_wifi(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    screen_style(scr);
    s_scr_wifi = scr;

    lv_obj_t *t = mk_label(scr, "选择热点", &lv_font_sc14, UI_TEXT);
    lv_obj_set_pos(t, 6, 3);
    lv_obj_t *h = mk_label(scr, "B:重扫", &lv_font_sc12, UI_DIM);
    lv_obj_align(h, LV_ALIGN_TOP_RIGHT, -6, 5);

    for (int i = 0; i < LIST_ROWS; i++) {
        lv_obj_t *r = lv_obj_create(scr);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 152, 17);
        lv_obj_set_pos(r, 4, 22 + i * 17);
        lv_obj_set_style_radius(r, 3, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        s_row[i] = r;
        s_row_name[i] = mk_label(r, "", &lv_font_sc14, UI_TEXT);
        lv_obj_set_width(s_row_name[i], 116);
        lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 4, 0);
        s_row_info[i] = mk_label(r, "", &lv_font_sc12, UI_DIM);
        lv_obj_align(s_row_info[i], LV_ALIGN_RIGHT_MID, -4, 0);
    }

    s_wifi_msg = mk_label(scr, "", &lv_font_sc14, UI_TEXT);
    lv_obj_align(s_wifi_msg, LV_ALIGN_CENTER, 0, 6);
    lv_obj_add_flag(s_wifi_msg, LV_OBJ_FLAG_HIDDEN);
}

/* 网格第 0 行：大小写 / 删除 / 确认；1~5 行：字符 */
static const char *GRID_CH[5][10] = {
    {"a","b","c","d","e","f","g","h","i","j"},
    {"k","l","m","n","o","p","q","r","s","t"},
    {"u","v","w","x","y","z","0","1","2","3"},
    {"4","5","6","7","8","9","-","_",".","@"},
    {"#","!","$","%","&","*","+","=","?","/"},
};

static int row_ncols(int r) { return r == 0 ? 3 : 10; }

static void cell_text(int r, int c, char *out, size_t n) {
    if (r == 0) {
        const char *t[3] = { "Aa", "删除", "确认" };
        strlcpy(out, t[c], n);
        return;
    }
    const char *s = GRID_CH[r-1][c];
    if (s_shift && s[0] >= 'a' && s[0] <= 'z') {
        snprintf(out, n, "%c", s[0] - 'a' + 'A');
    } else {
        strlcpy(out, s, n);
    }
}

static void build_pass(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    screen_style(scr);
    s_scr_pass = scr;

    s_pw_ssid = mk_label(scr, "", &lv_font_sc12, UI_DIM);
    lv_obj_set_pos(s_pw_ssid, 6, 1);
    lv_obj_set_width(s_pw_ssid, 148);                 /* 长 SSID 截断，别出屏 */
    lv_label_set_long_mode(s_pw_ssid, LV_LABEL_LONG_DOT);
    s_pw_text = mk_label(scr, "_", &lv_font_sc14, UI_TEXT);
    lv_obj_set_pos(s_pw_text, 6, 15);

    for (int r = 0; r < 6; r++) {
        int n = row_ncols(r);
        int w = 160 / n;
        for (int c = 0; c < n; c++) {
            lv_obj_t *o = lv_obj_create(scr);
            lv_obj_remove_style_all(o);
            lv_obj_set_size(o, w - 1, CELL_H - 1);
            lv_obj_set_pos(o, c * w + (160 - n * w) / 2, CELL_Y0 + r * CELL_H);
            lv_obj_set_style_radius(o, 2, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = mk_label(o, "", &lv_font_sc12, UI_TEXT);
            lv_obj_center(l);
            s_cell[r][c] = o;
            s_cell_lbl[r][c] = l;
        }
    }

    s_pw_msg = mk_label(scr, "", &lv_font_sc14, C_YELLOW);
    lv_obj_align(s_pw_msg, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(s_pw_msg, LV_OBJ_FLAG_HIDDEN);
}

static void pw_msg_show(const char *txt, uint32_t ms) {
    lv_label_set_text(s_pw_msg, txt);
    lv_obj_remove_flag(s_pw_msg, LV_OBJ_FLAG_HIDDEN);
    s_msg_until = lv_tick_get() + ms;
}

static void pass_refresh(void) {
    char buf[80], t[8];
    const char *p = s_pw;
    if (s_pwlen > 17) p = s_pw + (s_pwlen - 17);
    snprintf(buf, sizeof buf, "%s_", p);
    lv_label_set_text(s_pw_text, buf);

    for (int r = 0; r < 6; r++) {
        int n = row_ncols(r);
        for (int c = 0; c < n; c++) {
            cell_text(r, c, t, sizeof t);
            lv_label_set_text(s_cell_lbl[r][c], t);
            bool sel = (r == s_grid_row && c == s_grid_col);
            bool shift_on = (r == 0 && c == 0 && s_shift);
            uint32_t bg = sel ? C_YELLOW : (shift_on ? UI_SEL : 0);
            if (sel || shift_on) {
                lv_obj_set_style_bg_color(s_cell[r][c], lv_color_hex(bg), 0);
                lv_obj_set_style_bg_opa(s_cell[r][c], LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_opa(s_cell[r][c], LV_OPA_TRANSP, 0);
            }
            lv_obj_set_style_text_color(s_cell_lbl[r][c],
                lv_color_hex(sel ? UI_BG : UI_TEXT), 0);
        }
    }
}

static void wifi_refresh(void) {
    bool msg_mode = s_scanning || s_connecting || s_msg_until;
    if (msg_mode) {
        if (s_connecting)      strlcpy(s_msg, "连接中...", sizeof s_msg);
        else if (s_scanning)   strlcpy(s_msg, "扫描中...", sizeof s_msg);
        for (int i = 0; i < LIST_ROWS; i++)
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_wifi_msg, s_msg);
        lv_obj_remove_flag(s_wifi_msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_wifi_msg, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < LIST_ROWS; i++)
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + LIST_ROWS) s_scroll = s_cursor - LIST_ROWS + 1;

    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = s_scroll + i;
        bool sel = (idx == s_cursor);
        lv_obj_set_style_bg_opa(s_row[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) lv_obj_set_style_bg_color(s_row[i], lv_color_hex(UI_SEL), 0);
        if (idx < s_nap) {
            char info[12];
            snprintf(info, sizeof info, "%d%s", s_ap[idx].rssi,
                     s_ap[idx].auth == WIFI_AUTH_OPEN ? "" : "*");
            lv_label_set_text(s_row_name[i], s_ap[idx].ssid);
            lv_label_set_text(s_row_info[i], info);
        } else if (idx == s_nap) {
            lv_label_set_text(s_row_name[i], "< 返回");
            lv_label_set_text(s_row_info[i], "");
        } else {
            lv_label_set_text(s_row_name[i], "");
            lv_label_set_text(s_row_info[i], "");
        }
        lv_obj_set_style_text_color(s_row_name[i],
            lv_color_hex(sel ? UI_TEXT : UI_DIM), 0);
    }
}

/* 授权候选界面：Claude 要授权（黄灯 NEEDS YOU）时按 A 进来。
 * 方向键不做光标移动——直接就是决定，一键到位，少按一次。
 * 左右两个是「永久性」的决定（写白名单 / 拒绝），用绿红区分。 */
static const struct { uint8_t key; int dec; const char *txt; uint32_t col; } ASK_ROWS[] = {
    { EV_UP,    DEC_ALLOW,  "↑ 继续",           UI_TEXT },
    { EV_DOWN,  DEC_RETRY,  "↓ 重来",           UI_TEXT },
    { EV_LEFT,  DEC_ALWAYS, "← 总是接受更改",   C_GREEN },
    { EV_RIGHT, DEC_DENY,   "→ 拒绝更改或指令", C_RED   },
};

static void build_ask(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    screen_style(scr);
    s_scr_ask = scr;

    lv_obj_t *t = mk_label(scr, "NEEDS YOU !", &lv_font_montserrat_14, C_YELLOW);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 3);

    /* 问句：这次要授权的到底是什么（工具 + 关键参数）。
     * 最多 3 行，超长由 ask_refresh 截断；没推问句就显示占位提示。 */
    s_lbl_ask_q = mk_label(scr, "", &lv_font_sc12, UI_DIM);
    lv_obj_set_pos(s_lbl_ask_q, 8, 22);
    lv_obj_set_width(s_lbl_ask_q, 146);
    lv_label_set_long_mode(s_lbl_ask_q, LV_LABEL_LONG_DOT);

    /* 四个选项：sc12 字体、行高 17，从 y=62 起（62+68=130 压线，
     * 最后一行 62+3*17=113，字高 12 → 底 125，放得下） */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = mk_label(scr, ASK_ROWS[i].txt, &lv_font_sc12, ASK_ROWS[i].col);
        lv_obj_set_pos(l, 8, 62 + i * 17);
    }
}

/* ask 界面内容随 s_ask_slot 刷新：问句 + 该槽的摘要兜底 */
static void ask_refresh(int slot) {
    const char *q = (slot >= 0 && slot < SESS_MAX) ? s_sessions[slot].question : "";
    if (!q[0]) q = "（电脑没说是什么操作）";
    lv_label_set_text(s_lbl_ask_q, q);
}

/* ── 设置菜单（首页短按 ↑ 进入）─────────────────────────────────────────
 * 每行左边是名字、右边是当前值；选中行前面挂一个 ">" 并用黄色标出来。
 * 用文字标记而不是只改颜色，是因为 QEMU 的字符画快照看不出颜色，
 * 挂个字符才能在快照里验证「光标到底停在第几行」。 */
#define MENU_ITEMS 4
static const char *MENU_NAME[MENU_ITEMS] = {
    "显示 IP",      /* 0：底部一行在「摘要 ↔ IP」之间切 */
    "声音",         /* 1：蜂鸣器开关 */
    "连接",         /* 2：WiFi ↔ 蓝牙（要重启生效）*/
    "重设 WiFi",    /* 3：进扫描界面重连 */
};
static lv_obj_t *s_menu_name[MENU_ITEMS];
static lv_obj_t *s_menu_val[MENU_ITEMS];
static lv_obj_t *s_menu_msg;
static int       s_menu_sel;

/* 切完「连接」不立刻重启，先亮一下提示再重启，否则用户不知道发生了什么 */
static uint32_t s_restart_at;

static void menu_refresh(void) {
    char b[32];
    for (int i = 0; i < MENU_ITEMS; i++) {
        bool sel = (i == s_menu_sel);
        snprintf(b, sizeof b, "%s %s", sel ? ">" : " ", MENU_NAME[i]);
        lv_label_set_text(s_menu_name[i], b);
        lv_obj_set_style_text_color(s_menu_name[i],
                                    lv_color_hex(sel ? C_YELLOW : UI_TEXT), 0);

        const char *v = "";
        switch (i) {
        case 0: v = s_foot_show_ip ? "开" : "关"; break;
        case 1: v = beep_enabled()  ? "开" : "关"; break;
        case 2: v = s_ble_mode      ? "蓝牙" : "WiFi"; break;
        default: break;
        }
        lv_label_set_text(s_menu_val[i], v);
        lv_obj_set_style_text_color(s_menu_val[i],
                                    lv_color_hex(sel ? C_YELLOW : UI_DIM), 0);
    }
    if (!s_restart_at) lv_obj_add_flag(s_menu_msg, LV_OBJ_FLAG_HIDDEN);
}

static void build_menu(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    screen_style(scr);
    s_scr_menu = scr;

    lv_obj_t *t = mk_label(scr, "设置", &lv_font_sc14, UI_TEXT);
    lv_obj_set_pos(t, 4, 3);

    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = 26 + i * 22;
        s_menu_name[i] = mk_label(scr, "", &lv_font_sc14, UI_TEXT);
        lv_obj_set_pos(s_menu_name[i], 8, y);
        s_menu_val[i] = mk_label(scr, "", &lv_font_sc14, UI_DIM);
        lv_obj_align(s_menu_val[i], LV_ALIGN_TOP_RIGHT, -8, y);
    }

    s_menu_msg = mk_label(scr, "RESTART", &lv_font_montserrat_14, C_YELLOW);
    lv_obj_align(s_menu_msg, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_menu_msg, LV_OBJ_FLAG_HIDDEN);

    menu_refresh();
}

static void ui_show(scr_t s) {
    s_scr = s;
    if (s == SCR_HOME)       lv_screen_load(s_scr_home);
    else if (s == SCR_ASK)   lv_screen_load(s_scr_ask);
    else if (s == SCR_MENU)  {
        /* 每次进来都回到第一项：光标留在「重设 WiFi」上的话，下次进来
         * 顺手一按 A 就把 WiFi 重扫了，太容易误触。 */
        s_menu_sel = 0;
        menu_refresh();
        lv_screen_load(s_scr_menu);
    }
    else if (s == SCR_WIFI) { wifi_refresh(); lv_screen_load(s_scr_wifi); }
    else {
        char b[48];
        snprintf(b, sizeof b, "密码: %s", s_sel_ssid);
        lv_label_set_text(s_pw_ssid, b);
        lv_obj_add_flag(s_pw_msg, LV_OBJ_FLAG_HIDDEN);   /* 清掉上次的提示 */
        s_msg_until = 0;
        pass_refresh();
        lv_screen_load(s_scr_pass);
    }
}

static void msg_show(const char *txt, uint32_t ms) {
    strlcpy(s_msg, txt, sizeof s_msg);
    s_msg_until = lv_tick_get() + ms;
    wifi_refresh();
}

/* ── 会话槽操作（HTTP / BLE 任务上下文调用：只改状态，渲染全在主循环）──
 * 这些函数会被 httpd / ble worker 的任务调到，所以只碰普通内存和 LEDC
 * 寄存器，绝不碰 LVGL 对象。 */

/* 聚合态：黄 > 红 > 绿 > 灭，最需要人管的赢。
 * 返回聚合态；*slot_out（可空）给「该状态归属哪个槽」（同状态取最小槽号）。*/
static light_t agg_state(volatile int *slot_out) {
    static const light_t prio[] = { ST_YELLOW, ST_RED, ST_GREEN };
    for (unsigned p = 0; p < sizeof prio / sizeof prio[0]; p++)
        for (int i = 0; i < SESS_MAX; i++)
            if (s_sessions[i].st == prio[p]) {
                if (slot_out) *slot_out = i;
                return prio[p];
            }
    if (slot_out) *slot_out = 0;
    return ST_OFF;
}

/* 槽位状态迁移。蜂鸣只在**聚合态**跨越黄/绿边界时响——多会话下 B 会话
 * 的黄灯不该因为 A 会话也在黄灯就多响一轮。 */
static void sess_set(int slot, light_t st) {
    if (slot < 0 || slot >= SESS_MAX) slot = 0;
    sess_t *s = &s_sessions[slot];
    light_t before = agg_state(NULL);
    s->st = st;
    s->alert_fin = false;
    switch (st) {
    case ST_RED:                        /* 开始干活：起表 */
        s->run_start = lv_tick_get();
        s->run_ms = 0;
        s->run_live = true;
        break;
    case ST_GREEN:                      /* 任务完成：冻结计时 */
        if (s->run_live) { s->run_ms = lv_tick_get() - s->run_start; s->run_live = false; }
        break;
    case ST_YELLOW:
        break;                          /* 30s 到点由主循环按槽记（见 alert_fin）*/
    default:                            /* off */
        s->run_live = false;
        s->run_ms = 0;
        break;
    }
    light_t after = agg_state(NULL);
    if (after != before) {
        if (after == ST_YELLOW) {
            beep_two();                 /* 需要你操作：滴 滴 */
            s_alert_on = true;
            s_alert_start = lv_tick_get();
            s_blink_next = 0;
            s_blink_fase = false;
        } else if (before == ST_YELLOW) {
            s_alert_on = false;
        }
        if (after == ST_GREEN && before != ST_GREEN) beep_two();
    }
}

/* 兼容旧调用点（QEMU 按键直驱、lc 回调等，全是槽 0）*/
static inline void set_state(light_t st) { sess_set(0, st); }

static void render_state(light_t st) {
    mochi_draw(mochi_of(st));

    lv_label_set_text(s_lbl_state, state_text(st));
    uint32_t tc = (st == ST_RED) ? 0x30180C : (st == ST_YELLOW) ? 0x8A6A00
                : (st == ST_GREEN) ? 0x14520F : 0x8A4A2A;
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(tc), 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(tc), 0);   /* 计时器同色 */
}

/* ── 交互逻辑 ─────────────────────────────────────────────────────────── */
static void start_connect(void) {    s_connecting = true;
    wifi_req_connect(s_sel_ssid, s_pw);
    if (s_scr == SCR_PASS) { lv_screen_load(s_scr_wifi); s_scr = SCR_WIFI; }
    wifi_refresh();
}

static void wifi_key(uint8_t code, bool lng) {
    if (s_scanning || s_connecting) return;
    if (s_msg_until) { s_msg_until = 0; wifi_refresh(); return; }
    int total = s_nap + 1;                       /* 末尾是"返回" */
    switch (code) {
    case EV_UP:
        s_cursor = (s_cursor + total - 1) % total;
        wifi_refresh();
        break;
    case EV_DOWN:
        s_cursor = (s_cursor + 1) % total;
        wifi_refresh();
        break;
    case EV_B:
        if (lng) { ui_show(SCR_HOME); break; }
        s_scanning = true;
        wifi_req_scan();
        wifi_refresh();
        break;
    case EV_A:
        if (s_cursor >= s_nap) { ui_show(SCR_HOME); break; }
        strlcpy(s_sel_ssid, s_ap[s_cursor].ssid, sizeof s_sel_ssid);
        if (s_ap[s_cursor].auth == WIFI_AUTH_OPEN) {
            s_pw[0] = 0; s_pwlen = 0;
            start_connect();
        } else {
            s_pw[0] = 0; s_pwlen = 0;
            s_grid_row = 1; s_grid_col = 0;
            ui_show(SCR_PASS);
        }
        break;
    default: break;
    }
}

static void pass_key(uint8_t code, bool lng) {
    int n = row_ncols(s_grid_row);
    switch (code) {
    case EV_UP:
        s_grid_row = (s_grid_row + 5) % 6;
        if (s_grid_col >= row_ncols(s_grid_row)) s_grid_col = 0;
        pass_refresh();
        break;
    case EV_DOWN:
        s_grid_row = (s_grid_row + 1) % 6;
        if (s_grid_col >= row_ncols(s_grid_row)) s_grid_col = 0;
        pass_refresh();
        break;
    case EV_LEFT:
        s_grid_col = (s_grid_col + n - 1) % n;
        pass_refresh();
        break;
    case EV_RIGHT:
        s_grid_col = (s_grid_col + 1) % n;
        pass_refresh();
        break;
    case EV_B:
        if (lng) {                                  /* 长按 B：退格 / 返回列表 */
            if (s_pwlen > 0) { s_pw[--s_pwlen] = 0; pass_refresh(); }
            else ui_show(SCR_WIFI);
        } else if (s_pwlen > 0) {
            s_pw[--s_pwlen] = 0;
            pass_refresh();
        }
        break;
    case EV_A:
        if (s_grid_row == 0) {
            if (s_grid_col == 0) { s_shift = !s_shift; pass_refresh(); }
            else if (s_grid_col == 1) { if (s_pwlen > 0) { s_pw[--s_pwlen] = 0; pass_refresh(); } }
            else if (s_pwlen > 0) start_connect();
            else pw_msg_show("请输入密码", 2000);
        } else if (s_pwlen < (int)sizeof s_pw - 1) {
            char t[8];
            cell_text(s_grid_row, s_grid_col, t, sizeof t);
            s_pw[s_pwlen++] = t[0];
            s_pw[s_pwlen] = 0;
            pass_refresh();
        }
        break;
    default: break;
    }
}

/* 候选界面按键：方向键直接做出决定，B 退回（不做决定）。
 * 决定写进当前正在回答的槽（s_ask_slot）；答完把该槽转红/绿——但如果
 * 别的槽还挂着 pending 的授权，聚合态仍会是黄，灯不会骗人。 */
static void ask_key(uint8_t code, bool lng) {
    if (lng) return;
    int slot = s_ask_slot;
    for (int i = 0; i < 4; i++) {
        if (code != ASK_ROWS[i].key) continue;
        int d = ASK_ROWS[i].dec;
        if (slot >= 0 && slot < SESS_MAX) {
            s_asks[slot].dec = d;
            sess_set(slot, (d == DEC_ALLOW || d == DEC_ALWAYS) ? ST_GREEN : ST_RED);
        }
        ui_show(SCR_HOME);
        return;
    }
    if (code == EV_B) ui_show(SCR_HOME);
}

/* 链路模式存 NVS，下次开机按它决定起 WiFi 还是蓝牙 */
static void link_mode_set(bool ble) {
    s_ble_mode = ble;
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "link", ble ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "链路模式 → %s（重启生效）", ble ? "蓝牙" : "WiFi");
}

static void link_mode_load(void) {
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "link", &v) == ESP_OK) s_ble_mode = v ? true : false;
    nvs_close(h);
}

/* 设置菜单：↑↓ 移光标，A 改这一项，B 退回首页 */
static void menu_key(uint8_t code, bool lng) {
    if (lng) return;
    if (s_restart_at) return;               /* 已经在重启倒计时里，别理按键 */

    if (code == EV_UP) {
        s_menu_sel = (s_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
        menu_refresh();
        return;
    }
    if (code == EV_DOWN) {
        s_menu_sel = (s_menu_sel + 1) % MENU_ITEMS;
        menu_refresh();
        return;
    }
    if (code == EV_B) { ui_show(SCR_HOME); return; }
    if (code != EV_A) return;

    switch (s_menu_sel) {
    case 0:
        s_foot_show_ip = !s_foot_show_ip;
        break;
    case 1:
        beep_set_enabled(!beep_enabled());
        if (beep_enabled()) beep_two();     /* 开的时候响一声，好确认真的开了 */
        break;
    case 2:
        /* 切链路要重新起 WiFi 栈或蓝牙栈，重启是最省事也最不容易出错的
         * 做法（ESP32 重启一秒不到）。先亮提示，主循环到点再真重启。*/
        link_mode_set(!s_ble_mode);
        lv_obj_remove_flag(s_menu_msg, LV_OBJ_FLAG_HIDDEN);
        s_restart_at = lv_tick_get() + 900;
        break;
    case 3:
        ui_show(SCR_WIFI);                  /* 和首页长按 ↑ 走同一条路 */
        s_scanning = true;
        wifi_req_scan();
        wifi_refresh();
        return;
    default:
        break;
    }
    menu_refresh();
}

static void handle_event(uint8_t ev) {
    bool lng = ev & EV_LONG;
    uint8_t code = ev & ~EV_LONG;
    switch (s_scr) {
    case SCR_HOME:
        if (code == EV_UP) {
            /* 长按 ↑ 是「直接进 WiFi 设置」的快捷方式（以前短按也走这里，
             * 现在短按改成进设置菜单了）*/
            if (lng) {
                ui_show(SCR_WIFI);
                s_scanning = true;
                wifi_req_scan();
                wifi_refresh();
            } else {
                ui_show(SCR_MENU);
            }
        }
        else if (code == EV_A && !lng && agg_state(NULL) == ST_YELLOW) {
            /* 黄灯（要授权）时按 A 进候选界面；回答哪个槽 =
             * 聚合态的归属槽（黄>红>绿，最需要人管的赢） */
            agg_state(&s_ask_slot);
            ask_refresh(s_ask_slot);
            ui_show(SCR_ASK);
        }
#if !CONFIG_TRAFFIC_ENABLE_WIFI
        /* QEMU 无 HTTP：另外几个键驱动状态，方便验证灯与文字 */
        else if (code == EV_DOWN && !lng)  set_state(ST_OFF);
        else if (code == EV_B && !lng)     set_state(ST_RED);
        else if (code == EV_RIGHT && !lng) set_state(ST_YELLOW);
        else if (code == EV_LEFT && !lng)  set_state(ST_GREEN);   /* 直接试绿灯路径 */
#endif
        break;
    case SCR_WIFI: wifi_key(code, lng); break;
    case SCR_PASS: pass_key(code, lng); break;
    case SCR_ASK:  ask_key(code, lng);  break;
    case SCR_MENU: menu_key(code, lng); break;
    }
}

#if !CONFIG_TRAFFIC_ENABLE_WIFI
/* 前置声明：demo 时间轴要用（定义在 WiFi/授权那一节）*/
static void sess_set_task(int slot, const char *txt);
static void sess_set_question(int slot, const char *txt);
static void ask_start(int slot, int timeout_s);

/* 多会话演示步骤：模拟两个多开的 Claude 各占一个槽。
 * sess_set/sess_set_task/sess_set_question/ask_start 正是 HTTP 与 BLE
 * 命令最终调用的函数，QEMU 里没有链路就直接调它们。 */
static void demo_ms1(void) {                /* 实例1：开始干活 */
    sess_set(1, ST_RED);
    sess_set_task(1, "修花屏并加授权候选界面");
}
static void demo_ms2(void) {                /* 实例2：要授权 */
    sess_set(2, ST_YELLOW);
    sess_set_task(2, "重构 HTTP 路由");
    sess_set_question(2, "Bash: rm -rf /tmp/build");
}

/* QEMU 自检：QEMU 的 esp32 机型收不到串口输入，改为按时间轴自动走一遍 UI，
 * 每步把屏幕打成字符画，用来在无硬件时肉眼验证界面与流程。
 * 走的是和真机按键完全相同的 handle_event() 路径。 */
static void demo_run(void) {
    typedef struct { uint32_t ms; int ev; const char *dump; void (*fn)(void); } step_t;
    static const step_t SEQ[] = {
        {  6000, EV_B|EV_LONG, NULL },          /* 长按B返回主界面 */
        {  8000, -1,         "1 主界面(熄灭)" },
        {  9000, EV_B,       NULL },            /* B = 红 */
        { 10000, -1,         "2 红灯 CLAUDE BUSY" },
        { 11000, EV_RIGHT,   NULL },            /* QEMU: RIGHT = 黄灯 */
        { 12000, -1,         "3 黄灯 NEEDS YOU" },
        { 13000, EV_UP|EV_LONG, NULL },         /* 长按进设置 → 扫描（短按会被"已连上"拦下）*/
        { 14000, -1,         "4 扫描中" },
        { 16000, -1,         "5 热点列表" },
        { 17000, EV_DOWN,    NULL },
        { 17100, EV_DOWN,    NULL },            /* 光标到 FreeWiFi(开放) */
        { 18000, -1,         "6 选中开放热点" },
        { 19000, EV_A,       NULL },            /* 选它 → 直接连 */
        { 20000, -1,         "7 连接中" },
        { 23000, -1,         "8 连上回主界面" },
        { 24000, EV_UP|EV_LONG, NULL },         /* 再进设置（长按）*/
        { 26500, EV_A,       NULL },            /* MyPhone(加密) → 密码界面 */
        { 27500, -1,         "9 密码输入界面" },
        { 28000, EV_A,       NULL },            /* 输入 a */
        { 28100, EV_RIGHT,   NULL },
        { 28200, EV_A,       NULL },            /* 输入 b */
        { 28300, EV_RIGHT,   NULL },
        { 28400, EV_A,       NULL },            /* 输入 c */
        { 29500, -1,         "10 已输入 abc" },
        { 30000, EV_B|EV_LONG, NULL },          /* 长按 B 退格 → ab */
        { 31000, -1,         "11 长按B退格后" },
        { 31500, EV_B,       NULL },            /* 短按 B 退格 → a */
        { 32500, -1,         "12 短按B退格后" },
        { 33000, EV_UP,      NULL },            /* 行0 列2 */
        { 33500, EV_LEFT,    NULL },            /* 行0 列1 = 删除 */
        { 34000, EV_A,       NULL },            /* 删除键 → 清空 */
        { 35000, -1,         "13 删除键清空" },
        { 35500, EV_DOWN,    NULL },            /* 回字符行 */
        { 36000, EV_A,       NULL },            /* 输入 b */
        { 37000, -1,         "14 重新输入 b" },
        { 37500, EV_UP,      NULL },            /* 行0 */
        { 38000, EV_RIGHT,   NULL },            /* 列2 = 确认 */
        { 38500, EV_A,       NULL },            /* 连接（密码非空→成功）*/
        { 40000, -1,         "15 连接中" },
        { 43000, -1,         "16 连上回主界面" },
        { 44000, EV_UP|EV_LONG, NULL },         /* 再进设置（长按，触发重扫）*/
        { 48000, EV_DOWN,    NULL },            /* 等扫描完，光标 → Xiaomi */
        { 50000, EV_A,       NULL },            /* 加密热点 → 密码界面（光标在 a）*/
        { 51000, EV_A,       NULL },            /* 输入 a */
        { 52000, -1,         "17 输入 a" },
        { 52500, EV_UP,      NULL },            /* 行0 列0 */
        { 53000, EV_RIGHT,   NULL },
        { 53500, EV_RIGHT,   NULL },            /* 列2 = 确认 */
        { 54000, EV_A,       NULL },            /* 连 Xiaomi（假装密码错）*/
        { 56000, -1,         "18 连接失败提示" },
        { 60000, -1,         "19 提示结束回列表" },
        /* 授权候选界面：黄灯 → A 进候选 → 方向键做决定 */
        { 62000, EV_B|EV_LONG, NULL },          /* 回主界面 */
        { 63000, EV_RIGHT,   NULL },            /* 黄灯 */
        { 64000, -1,         "20 黄灯 NEEDS YOU" },
        { 64500, EV_A,       NULL },            /* A 进候选界面 */
        { 65500, -1,         "21 候选词界面" },
        { 66000, EV_DOWN,    NULL },            /* ↓ 重来 → 拒绝重试 */
        { 67000, -1,         "22 按↓重来 转红灯" },
        { 67500, EV_RIGHT,   NULL },            /* 回黄灯 */
        { 68000, EV_A,       NULL },
        { 69000, -1,         "23 再进候选界面" },
        { 69500, EV_LEFT,    NULL },            /* ← 总是接受更改 → 绿灯 */
        { 70500, -1,         "24 按←总是接受 转绿灯" },
        { 71000, EV_RIGHT,   NULL },
        { 71500, EV_A,       NULL },
        { 72500, -1,         "25 候选界面(B 退回前)" },
        { 73000, EV_B,       NULL },            /* B 退回，不做决定 */
        { 74000, -1,         "26 按B退回主界面" },
        /* 设置菜单：短按 ↑ 进入，↑↓ 移光标，A 改这一项 */
        { 75000, EV_UP,      NULL },            /* 短按 ↑ → 设置菜单（长按才是直接进 WiFi）*/
        { 76000, -1,         "27 设置菜单(光标在 显示IP)" },
        { 76500, EV_A,       NULL },            /* 切「显示 IP」→ 开 */
        { 77500, -1,         "28 显示IP=开" },
        { 78000, EV_DOWN,    NULL },
        { 78500, EV_A,       NULL },            /* 切「声音」→ 关 */
        { 79500, -1,         "29 声音=关" },
        { 80000, EV_A,       NULL },            /* 再切回开 */
        { 80500, EV_DOWN,    NULL },
        { 81500, -1,         "30 光标在 连接(值应为 WiFi)" },
        { 82000, EV_DOWN,    NULL },
        { 83000, -1,         "31 光标在 重设WiFi" },
        { 83500, EV_B,       NULL },            /* 退回首页 */
        { 84500, EV_B,       NULL },            /* QEMU: B = 红灯 → 开始计时 */
        { 90500, -1,         "32 红灯计时中(应显示 6s)" },
        { 91000, EV_LEFT,    NULL },            /* QEMU: LEFT = 绿灯 → 停表 */
        { 92000, -1,         "33 绿灯计时冻结(仍是 6s)" },
        /* 链路模式切换：QEMU 里不真重启（重启会打断仿真），
         * 只验证 NVS 写入、值刷新和 RESTART 提示的出现与消失 */
        { 92500, EV_UP,      NULL },            /* 进设置菜单 */
        { 93500, -1,         "34 设置菜单(光标在 显示IP)" },
        { 94000, EV_DOWN,    NULL },
        { 94500, EV_DOWN,    NULL },            /* 光标到 连接 */
        { 95000, EV_A,       NULL },            /* 切 → 蓝牙 */
        { 95800, -1,         "35 连接=蓝牙 且 RESTART 提示" },
        { 96800, -1,         "36 提示已消失(连接仍是蓝牙)" },

        /* 多会话：两个 Claude 多开实例各占一个槽。QEMU 里没有 HTTP/BLE，
         * 直接调与两条链路共用的会话函数，走的路径和收到 @N 命令一致。 */
        { 97500, EV_B,       NULL },            /* 回主界面看聚合灯与轮播 */
        { 98000, 0, NULL, demo_ms1 },           /* 槽1 红灯 + 任务摘要 */
        { 99000, -1,         "37 多会话:槽1红灯(聚合红,摘要=槽1)" },
        {100000, 0, NULL, demo_ms2 },           /* 槽2 黄灯 + 任务摘要 + 问句 */
        {101000, -1,         "38 槽2黄灯(聚合黄,摘要轮播)" },
        {101500, -1,         "39 摘要轮播到槽2" },
        {102000, EV_A,       NULL },            /* 黄灯时按 A → ask 界面 */
        {103000, -1,         "40 ask界面(问句=槽2的授权问题)" },
        {104000, EV_UP,      NULL },            /* ↑ = 槽2 允许 → 转绿 */
        {105000, -1,         "41 槽2已答(聚合红回落,只剩槽1)" },
    };
    static int i = 0;
    static uint32_t t0 = 0;
    static uint32_t hb = 0;
    if (!t0) t0 = lv_tick_get();
    if ((int32_t)(lv_tick_get() - hb) >= 0) {
        hb = lv_tick_get() + 2000;
        ESP_LOGI(TAG, "hb t=%lu scr=%d state=%d scan=%d conn=%d msg=%lu step=%d",
                 (unsigned long)(lv_tick_get() - t0), s_scr, (int)agg_state(NULL),
                 s_scanning, s_connecting, (unsigned long)s_msg_until, i);
    }
    while (i < (int)(sizeof SEQ / sizeof SEQ[0]) &&
           (int32_t)(lv_tick_get() - t0) >= (int32_t)SEQ[i].ms) {
        if (SEQ[i].fn) {
            SEQ[i].fn();                    /* 多会话演示：直接驱动会话槽 */
        } else if (SEQ[i].ev >= 0) {
            handle_event((uint8_t)SEQ[i].ev);
        } else {
            printf("\n--- %s ---\n", SEQ[i].dump);
            fflush(stdout);
            s_dump_req = true;
            lv_obj_invalidate(lv_screen_active());
        }
        i++;
    }
}
#endif

/* ── HTTP ─────────────────────────────────────────────────────────────── */
/* 写一个槽的任务摘要（h_task 和 BLE 的 task 命令共用）。
 * 两个链路都要用，QEMU 自检也要直接调它演示多会话，所以不在 WiFi 块里。 */
static void sess_set_task(int slot, const char *txt) {
    if (slot < 0 || slot >= SESS_MAX) slot = 0;
    fit_text(s_sessions[slot].task, sizeof s_sessions[slot].task,
             txt ? txt : "", 19);
    ESP_LOGI(TAG, "task[%d]: %s", slot, s_sessions[slot].task);
}

/* 写一个槽的授权问句（h_question 和 BLE 的 question 命令共用），同样双链路 */
static void sess_set_question(int slot, const char *txt) {
    if (slot < 0 || slot >= SESS_MAX) slot = 0;
    if (txt && *txt) fit_text(s_sessions[slot].question,
                              sizeof s_sessions[slot].question, txt, 23);
    else             s_sessions[slot].question[0] = '\0';
    ESP_LOGI(TAG, "question[%d]: %s", slot, s_sessions[slot].question);
}

#if CONFIG_TRAFFIC_ENABLE_WIFI

/* 会话槽参数：?s=N，缺省 0。多开的几个 Claude 各占一个槽，
 * 不带参数的老命令全部落槽 0，单会话语义不变。 */
static int query_slot(httpd_req_t *req) {
    char q[32] = "", val[4] = "";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "s", val, sizeof val) == ESP_OK) {
        int s = atoi(val);
        if (s >= 0 && s < SESS_MAX) return s;
    }
    return 0;
}

static esp_err_t h_alerta(httpd_req_t *req) {
    sess_set(query_slot(req), ST_YELLOW);
    return httpd_resp_send(req, "ok alerta\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_rojo(httpd_req_t *req) {
    sess_set(query_slot(req), ST_RED);
    return httpd_resp_send(req, "ok solo rojo\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_verde(httpd_req_t *req) {
    sess_set(query_slot(req), ST_GREEN);
    return httpd_resp_send(req, "ok solo verde\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_off(httpd_req_t *req) {
    sess_set(query_slot(req), ST_OFF);
    return httpd_resp_send(req, "ok off\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_status(httpd_req_t *req) {
    static const char *names[] = {"off", "rojo", "amarillo", "verde"};
    char buf[320];
    int o = snprintf(buf, sizeof buf, "state: %s\nip: %s\nclk: %d\n",
             names[agg_state(NULL)], s_ipbuf, s_spi_hz / 1000000);
    for (int i = 0; i < SESS_MAX && o < (int)sizeof buf - 2; i++)
        if (s_sessions[i].st != ST_OFF || s_sessions[i].task[0])
            o += snprintf(buf + o, sizeof buf - o, "s%d: %s %s\n",
                          i, names[s_sessions[i].st], s_sessions[i].task);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* 把 %XX 还原成字节，'+' 还原成空格。esp_http_server 的
 * httpd_query_key_value() 只做切分，不做解码。 */
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* 摘要要进 label，滤掉控制字符（换行/Tab/ESC 会把一行撑乱） */
static void strip_ctl(char *s) {
    char *o = s;
    for (char *p = s; *p; p++)
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7F) *o++ = *p;
    *o = '\0';
}

/* POST /task  正文即摘要（UTF-8 原始字节）
 * GET  /task?text=...&s=N  便于 curl 手测，需 URL 编码 */
static esp_err_t h_task(httpd_req_t *req) {
    char buf[96] = "";
    int n = 0;
    if (req->method == HTTP_POST && req->content_len > 0) {
        n = req->content_len < (int)sizeof buf - 1 ? req->content_len : (int)sizeof buf - 1;
        if (httpd_req_recv(req, buf, n) <= 0) n = 0;
        buf[n] = '\0';
    } else {
        char q[160] = "";
        if (httpd_req_get_url_query_len(req) > 0 &&
            httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK)
            httpd_query_key_value(q, "text", buf, sizeof buf);
        url_decode(buf);
    }
    strip_ctl(buf);
    int slot = query_slot(req);
    sess_set_task(slot, buf);
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

/* ── 授权：登记 / 轮询（两段式，不占 httpd）─────────────────────────── */

/* start：把槽转黄（聚合态下别的槽的黄/红优先级不变），登记 pending 和
 * 截止时间，立即返回。真正等结果由 poll 一趟趟来。 */
static void ask_start(int slot, int timeout_s) {
    if (slot < 0 || slot >= SESS_MAX) slot = 0;
    if (timeout_s < 1)   timeout_s = 1;
    if (timeout_s > 120) timeout_s = 120;
    s_asks[slot].dec = DEC_NONE;
    s_asks[slot].pending = true;
    s_asks[slot].deadline = lv_tick_get() + (uint32_t)timeout_s * 1000;
    sess_set(slot, ST_YELLOW);
}

/* poll：拿到结果就清账返回；超时由**主循环**兜底清 pending（电脑侧中途
 * 掉线时不留幽灵请求）。返回值是 DEC_*，调用方翻译成名字。 */
static int ask_poll(int slot) {
    if (slot < 0 || slot >= SESS_MAX) return DEC_NONE;
    ask_t *a = &s_asks[slot];
    if (!a->pending) return DEC_NONE;
    int d = a->dec;
    if (d != DEC_NONE) {
        a->pending = false;
        a->dec = DEC_NONE;
    }
    return d;
}

/* 阻塞版：BLE 的 decision 命令用（跑在 ble worker 自己的任务里）。
 * WiFi 侧已经改走 start/poll，这条路留作蓝牙和手工测试。 */
static int decide_wait(int slot, int timeout_s) {
    static const char *names[] = { "none", "allow", "deny", "always", "retry" };
    ask_start(slot, timeout_s);
    if (slot < 0 || slot >= SESS_MAX) slot = 0;
    ask_t *a = &s_asks[slot];
    uint32_t deadline = a->deadline;
    while (a->pending && a->dec == DEC_NONE &&
           (int32_t)(lv_tick_get() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    int d = ask_poll(slot);
    if (d == DEC_NONE) d = 0;
    ESP_LOGI(TAG, "decision[%d]: %s", slot, names[d]);
    return d;
}

/* link_cmd 的宿主回调：HTTP 和蓝牙两条链路共用同一套动作。
 * 两个枚举的取值必须逐项相等，否则下面的强转就是错的——所以这里断言死。
 * （比较要转成 int：直接比两个不同的匿名枚举会被 -Werror=enum-compare 拦下）*/
_Static_assert((int)LC_OFF == (int)ST_OFF && (int)LC_RED == (int)ST_RED &&
               (int)LC_YELLOW == (int)ST_YELLOW && (int)LC_GREEN == (int)ST_GREEN,
               "lc_light_t 必须与 light_t 逐项对齐");
_Static_assert((int)LC_DEC_NONE == (int)DEC_NONE && (int)LC_DEC_ALLOW == (int)DEC_ALLOW &&
               (int)LC_DEC_DENY == (int)DEC_DENY && (int)LC_DEC_ALWAYS == (int)DEC_ALWAYS &&
               (int)LC_DEC_RETRY == (int)DEC_RETRY, "lc_dec_t 必须与 DEC_* 逐项对齐");

static void lc_set_light(int slot, lc_light_t st) { sess_set(slot, (light_t)st); }

static void lc_set_task(int slot, const char *txt) { sess_set_task(slot, txt); }

static void lc_set_question(int slot, const char *txt) { sess_set_question(slot, txt); }

static lc_dec_t lc_wait_decision(int slot, int timeout_s) {
    return (lc_dec_t)decide_wait(slot, timeout_s);
}

static const char *lc_status(void) {
    static const char *names[] = { "off", "rojo", "amarillo", "verde" };
    return names[agg_state(NULL)];
}

/* link [wifi|ble] —— 从电脑切掌机的链路模式，跟设置菜单里那一项同义。
 * 传参就切换（重启生效），不传参就报告当前值。 */
static const char *lc_switch_link(const char *arg) {
    static char msg[48];
    if (!*arg) {
        snprintf(msg, sizeof msg, "link: %s", s_ble_mode ? "ble" : "wifi");
        return msg;
    }
    bool want_ble;
    if (!strcasecmp(arg, "ble") || !strcasecmp(arg, "蓝牙")) want_ble = true;
    else if (!strcasecmp(arg, "wifi")) want_ble = false;
    else return "err: link wifi|ble";
    if (want_ble == s_ble_mode) {
        snprintf(msg, sizeof msg, "link: already %s", want_ble ? "ble" : "wifi");
        return msg;
    }
    link_mode_set(want_ble);
    lv_obj_remove_flag(s_menu_msg, LV_OBJ_FLAG_HIDDEN);
    s_restart_at = lv_tick_get() + 900;
    return "ok link, restart in 1s";
}

static const lc_host_t LC_HOST = {
    .set_light     = lc_set_light,
    .set_task      = lc_set_task,
    .set_question  = lc_set_question,
    .wait_decision = lc_wait_decision,
    .status        = lc_status,
    .switch_link   = lc_switch_link,
};

/* GET /decision?timeout=N  —— 长轮询等掌机上的按键。 */
/* 两段式授权（多开下 httpd 不再被长轮询占死）：
 *   /decision/start?timeout=N&s=N  → 登记，立即回 "ok"
 *   /decision/poll?s=N             → "pending" / "allow" / ... / "none"
 * 旧的一条式 /decision?timeout=N 保留（阻塞版），蓝牙路径和手测用。 */
static esp_err_t h_decision_start(httpd_req_t *req) {
    int timeout = 60;
    char q[64] = "", val[8] = "";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "timeout", val, sizeof val) == ESP_OK)
        timeout = atoi(val);
    ask_start(query_slot(req), timeout);
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

/* 阻塞版（蓝牙路径、手测）：占住 httpd 直到有决定或超时。
 * 钩子走的是 start/poll，别用这条。 */
static esp_err_t h_decision(httpd_req_t *req) {
    static const char *names[] = { "none", "allow", "deny", "always", "retry" };
    int timeout = 60;
    char q[64] = "", val[8] = "";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "timeout", val, sizeof val) == ESP_OK)
        timeout = atoi(val);
    char buf[16];
    snprintf(buf, sizeof buf, "%s\n", names[decide_wait(query_slot(req), timeout)]);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_decision_poll(httpd_req_t *req) {
    static const char *names[] = { "none", "allow", "deny", "always", "retry" };
    char buf[16];
    int slot = query_slot(req);
    int d = ask_poll(slot);
    if (d == DEC_NONE && slot >= 0 && slot < SESS_MAX && s_asks[slot].pending)
        snprintf(buf, sizeof buf, "pending\n");
    else
        snprintf(buf, sizeof buf, "%s\n", names[d]);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* 授权问句：这次要授权的到底是什么操作。ask 界面黄灯时显示。 */
static esp_err_t h_question(httpd_req_t *req) {
    char buf[96] = "";
    int n = 0;
    if (req->method == HTTP_POST && req->content_len > 0) {
        n = req->content_len < (int)sizeof buf - 1 ? req->content_len : (int)sizeof buf - 1;
        if (httpd_req_recv(req, buf, n) <= 0) n = 0;
        buf[n] = '\0';
    } else {
        char q[160] = "";
        if (httpd_req_get_url_query_len(req) > 0 &&
            httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK)
            httpd_query_key_value(q, "text", buf, sizeof buf);
        url_decode(buf);
    }
    strip_ctl(buf);
    lc_set_question(query_slot(req), buf);
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

/* GET /link          → 当前链路模式
 * GET /link?mode=ble → 切到蓝牙（重启生效）；mode=wifi 同理 */
static esp_err_t h_link(httpd_req_t *req) {
    char q[64] = "", val[8] = "";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "mode", val, sizeof val) == ESP_OK) {
        const char *r = lc_switch_link(val);
        return httpd_resp_send(req, r, strlen(r));
    }
    char out[64];
    snprintf(out, sizeof out, "link: %s\n", s_ble_mode ? "ble" : "wifi");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 18;    /* 默认 8；现在有 17 条 URI */
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t uris[] = {
        { .uri = "/alerta",        .method = HTTP_POST, .handler = h_alerta },
        { .uri = "/alerta",        .method = HTTP_GET,  .handler = h_alerta },
        { .uri = "/solo/rojo",     .method = HTTP_POST, .handler = h_rojo   },
        { .uri = "/solo/rojo",     .method = HTTP_GET,  .handler = h_rojo   },
        { .uri = "/solo/verde",    .method = HTTP_POST, .handler = h_verde  },
        { .uri = "/solo/verde",    .method = HTTP_GET,  .handler = h_verde  },
        { .uri = "/off",           .method = HTTP_POST, .handler = h_off    },
        { .uri = "/off",           .method = HTTP_GET,  .handler = h_off    },
        { .uri = "/status",        .method = HTTP_GET,  .handler = h_status },
        { .uri = "/task",          .method = HTTP_POST, .handler = h_task   },
        { .uri = "/task",          .method = HTTP_GET,  .handler = h_task   },
        { .uri = "/question",      .method = HTTP_POST, .handler = h_question },
        { .uri = "/question",      .method = HTTP_GET,  .handler = h_question },
        { .uri = "/decision",      .method = HTTP_GET,  .handler = h_decision },
        { .uri = "/decision/start",.method = HTTP_GET,  .handler = h_decision_start },
        { .uri = "/decision/poll", .method = HTTP_GET,  .handler = h_decision_poll },
        { .uri = "/link",          .method = HTTP_GET,  .handler = h_link   },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
}
#endif

/* ── WiFi 结果处理（主循环上下文，可安全操作 LVGL）────────────────────── */
static void wifi_result(const wres_t *r) {
    if (r->kind == WRES_SCAN) {
        s_scanning = false;
        s_nap = r->nap;
        memcpy(s_ap, r->ap, sizeof s_ap);
        s_cursor = 0;
        s_scroll = 0;
        if (s_scr == SCR_WIFI) {
            if (s_nap == 0) msg_show("未找到热点", 2000);
            else wifi_refresh();
        }
    } else {
        s_connecting = false;
        if (r->ok) {
            s_wifi_ok = true;
            cfg_save(s_sel_ssid, s_pw);
            s_pwlen = 0; s_pw[0] = 0;
            ui_show(SCR_HOME);
        } else {
            if (s_scr != SCR_WIFI) { lv_screen_load(s_scr_wifi); s_scr = SCR_WIFI; }
            msg_show("连接失败", 2500);
        }
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

/* 启动时把内存账打出来。帧缓冲的两条死法都不好从现象上区分：
 * 拿不到会走 assert 进重启循环；spi_master 拿不到临时 DMA 缓冲则是每帧报
 * 0x101、屏幕整个不刷新（但板子活着）。先记一笔，现场就能对上号。*/
static void boot_mem_report(void) {
    ESP_LOGW(TAG, "heap: 内部 %u/最大块 %u，PSRAM %u/最大块 %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void app_main(void) {
    return_to_loader_setup();        /* 必须第一行 */

    ESP_LOGI(TAG, "boot");
    boot_mem_report();

    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    input_init();
    beep_load();                     /* 声音开关（NVS），要先于 beep_init */
    beep_init();
    link_mode_load();                /* 起 WiFi 还是起蓝牙 */
    esp_lcd_panel_io_handle_t io = lcd_init();


    lv_init();
    lv_display_t *disp = display_init(io);
    (void)disp;

    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, LVGL_TICK_PERIOD_MS * 1000);

    build_home();
    build_wifi();
    build_pass();
    build_ask();
    build_menu();
    render_state(ST_OFF);            /* 把 "BOOT..." 换成实际初始状态 "OFF" */
    ESP_LOGI(TAG, "ui built");
    lv_screen_load(s_scr_home);
    lv_refr_now(NULL);
#if CONFIG_TRAFFIC_ENABLE_WIFI
    /* 首帧已写入显存后打开显示（DISPON）——不开的话面板保持白屏背光 */
    st_tx(io, ST7735_DISPON, NULL, 0);
    st_delay(20);
#endif
    ESP_LOGI(TAG, "first frame flushed");

    s_wreq = xQueueCreate(4, sizeof(wreq_t));
    s_wres = xQueueCreate(4, sizeof(wres_t));

#if CONFIG_TRAFFIC_ENABLE_BLE
    if (s_ble_mode) {
        /* 蓝牙模式：不起 WiFi（也就没有热点和 IP），直接起 GATT 外设。
         * 两种模式二选一，所以不存在 WiFi / BT 抢射频的共存问题。*/
        ble_link_start(&LC_HOST);
        ESP_LOGI(TAG, "链路模式: 蓝牙");
        ui_show(SCR_HOME);
    } else
#endif
    {
        ESP_LOGI(TAG, "链路模式: WiFi");
        wifi_start();
        xTaskCreate(wifi_worker, "wifi", 4096, NULL, 5, NULL);
#if CONFIG_TRAFFIC_ENABLE_WIFI
        start_webserver();
#endif

        /* 有存档就直接连，没有就进设置界面 */
        char ssid[33], pass[64];
        ui_show(SCR_WIFI);
        if (cfg_load(ssid, sizeof ssid, pass, sizeof pass)) {
            ESP_LOGI(TAG, "saved ssid: %s", ssid);
            strlcpy(s_sel_ssid, ssid, sizeof s_sel_ssid);
            strlcpy(s_pw, pass, sizeof s_pw);
            s_pwlen = strlen(s_pw);
            s_connecting = true;
            wifi_refresh();
            wifi_req_connect(ssid, pass);
        } else {
            s_scanning = true;
            wifi_req_scan();
            wifi_refresh();
        }
    }

    /* 主循环：消费事件/结果 → LVGL 渲染（LVGL 单线程访问）*/
    light_t  last = ST_OFF;
    bool     last_blink = false;
    char     foot_shown[96] = "\x01";   /* 初值不可能等于任何真实内容，逼首帧刷一次 */
    char     time_shown[16] = "\x01";
    while (true) {
        input_scan();
        uint8_t ev;
        while (ev_pop(&ev)) handle_event(ev);
        mochi_tick();                    /* cc-mochi 表情动画（呼吸/眨眼/瞳孔）*/
        beep_tick();                     /* 推进蜂鸣器音序（非阻塞）*/

        /* 设置菜单里切了链路模式：提示亮完就真重启 */
        if (s_restart_at && (int32_t)(lv_tick_get() - s_restart_at) >= 0) {
            ESP_LOGW(TAG, "重启以切换链路模式");
#if CONFIG_TRAFFIC_ENABLE_WIFI
            esp_restart();
#else
            s_restart_at = 0;            /* QEMU 里重启会打断仿真，只记一笔 */
            lv_obj_add_flag(s_menu_msg, LV_OBJ_FLAG_HIDDEN);
            menu_refresh();
#endif
        }

#if !CONFIG_TRAFFIC_ENABLE_WIFI
        /* QEMU 验证：把当前界面整屏重绘一次，flush_cb 里打印字符画 */
        if (s_dump_req) lv_obj_invalidate(lv_screen_active());
        demo_run();
#endif

        wres_t r;
        while (xQueueReceive(s_wres, &r, 0) == pdTRUE) wifi_result(&r);

        if (s_msg_until && (int32_t)(lv_tick_get() - s_msg_until) >= 0) {
            s_msg_until = 0;
            if (s_scr == SCR_WIFI) wifi_refresh();
            else if (s_scr == SCR_PASS) lv_obj_add_flag(s_pw_msg, LV_OBJ_FLAG_HIDDEN);
        }

        /* 底部一行：默认轮播各会话的任务摘要（多开时每 4 秒换一路），
         * 短按 ↑ 切成 IP。内容变了才重设 label，否则每帧 set_text 会
         * 白白让整屏重绘 */
        char want[96];
        if (s_foot_show_ip && s_wifi_ok) {
            snprintf(want, sizeof want, "http://%s", s_ipbuf);
        } else {
            /* 轮播优先级：黄 > 红 > 绿 > 灭；同轮内从上次槽的下一个找。
             * 只轮播还活跃（非 off）的槽——off 的槽摘要保留着但不再显示，
             * 否则全部会话结束后底部还在滚动最近一次的任务文本。 */
            static int rot = 0;
            static uint32_t rot_at = 0;
            const char *pick = s_wifi_ok ? "空闲中" : "未连接";
            if (lv_tick_get() - rot_at >= 4000) {
                rot_at = lv_tick_get();
                for (int k = 0; k < SESS_MAX; k++) {
                    int i = (rot + 1 + k) % SESS_MAX;
                    if (s_sessions[i].st != ST_OFF && s_sessions[i].task[0]) {
                        rot = i;
                        break;
                    }
                }
            }
            if (s_sessions[rot].st != ST_OFF && s_sessions[rot].task[0])
                pick = s_sessions[rot].task;
            else if (rot != 0) {
                rot = 0;    /* 回到槽 0 待命，下次有活跃槽从它附近找 */
            }
            strlcpy(want, pick[0] ? pick : (s_wifi_ok ? "空闲中" : "未连接"),
                    sizeof want);
        }
        if (strcmp(want, foot_shown)) {
            strlcpy(foot_shown, want, sizeof foot_shown);
            foot_set(want);
        }

        /* 计时器：显示聚合优先槽的耗时；和底部一行一样做 dirty-check */
        int agg_slot = 0;
        light_t st = agg_state(&agg_slot);
        char tbuf[16] = "";
        if (s_sessions[agg_slot].run_live || s_sessions[agg_slot].run_ms)
            run_text(tbuf, sizeof tbuf, agg_slot);
        if (strcmp(tbuf, time_shown)) {
            strlcpy(time_shown, tbuf, sizeof time_shown);
            lv_label_set_text(s_lbl_time, tbuf);
        }

        bool blink_phase = true;
        if (st == ST_YELLOW && s_alert_on) {
            uint32_t now = lv_tick_get();
            /* 过期的授权请求先清账：电脑侧 start 登记的 deadline 到了没人按，
             * 就把 pending 撤掉（poll 会得到 none）——不然 pending 一直挂着，
             * 下面的 30s 自动熄灯被 any_pending 永久挡住，黄灯不灭。 */
            for (int i = 0; i < SESS_MAX; i++)
                if (s_asks[i].pending && (int32_t)(now - s_asks[i].deadline) >= 0) {
                    s_asks[i].pending = false;
                    ESP_LOGI(TAG, "decision[%d] 超时无人按，撤销 pending", i);
                }
            /* 有钩子在等授权结果时不许自动熄灭，否则用户还没按键灯就没了。
             * 黄灯 30s 到点按槽记账（sess_set 每次清，这里对黄灯槽统一判）。*/
            bool any_pending = false;
            for (int i = 0; i < SESS_MAX; i++)
                if (s_asks[i].pending) any_pending = true;
            if (now >= s_blink_next) {
                s_blink_fase = !s_blink_fase;
                s_blink_next = now + BLINK_MS;
            }
            blink_phase = s_blink_fase;
            if (!any_pending && now - s_alert_start >= ALERTA_MS) {
                /* 30 秒无人理会：把还黄着的槽全部熄灭 */
                for (int i = 0; i < SESS_MAX; i++)
                    if (s_sessions[i].st == ST_YELLOW) sess_set(i, ST_OFF);
                st = agg_state(&agg_slot);
            }
        }

        bool need = false;
        if (st != last) need = true;
        if (st == ST_YELLOW && blink_phase != last_blink) need = true;
        if (need) {
            last_blink = blink_phase;
            render_state(st);
        }
        last = st;

        uint32_t delay = lv_timer_handler();
        usleep(MAX(MIN(delay, 16), 1) * 1000);
    }
}
