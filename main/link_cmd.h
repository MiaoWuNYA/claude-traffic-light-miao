/* link_cmd.h — 掌机「链路命令」的纯逻辑层
 *
 * WiFi 那条路走 HTTP（main.c 里的 h_* handler），蓝牙那条路走 BLE GATT 写。
 * 两条路的**语义必须完全一致**，否则换个连接方式行为就变了。所以把
 * 「解析命令 → 调动作 → 出回执」这段抽到这里：不碰 ESP-IDF、不碰 LVGL、
 * 不碰任何全局状态，动作全部通过 lc_host_t 回调出去。
 *
 * 好处是这一层可以用电脑上的 gcc 直接单测（test/test_link_cmd.c），
 * 而 QEMU 的 esp32 机型没有蓝牙模型，BLE 那条路本来就没法仿真。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 灯状态，与 main.c 的 light_t 一一对应 */
typedef enum {
    LC_OFF = 0,
    LC_RED,
    LC_YELLOW,
    LC_GREEN,
} lc_light_t;

/* 授权决定，与 main.c 的 DEC_* 一一对应 */
typedef enum {
    LC_DEC_NONE = 0,   /* 超时 / 没人按键 */
    LC_DEC_ALLOW,
    LC_DEC_DENY,
    LC_DEC_ALWAYS,
    LC_DEC_RETRY,
} lc_dec_t;

/* 宿主（main.c）提供的动作。任何一项都可以为 NULL，lc_exec 会跳过。*/
typedef struct {
    void      (*set_light)(lc_light_t st);
    void      (*set_task)(const char *txt);
    lc_dec_t  (*wait_decision)(int timeout_s);   /* 阻塞等掌机按键 */
    const char *(*status)(void);                 /* 返回静态串，如 "rojo" */
    const char *(*switch_link)(const char *arg); /* "wifi"/"ble"：切换链路并安排重启；
                                                    返回说明文字（复用为回执正文）*/
} lc_host_t;

#define LC_DEC_TIMEOUT_DEFAULT 60
#define LC_DEC_TIMEOUT_MAX     120

/* 解析并执行一条命令。回执（含结尾换行）写进 out。
 *
 * 返回 true  = 认出了这条命令，out 里有回执（成功或错误说明）。
 * 返回 false = 不认识的命令，out 未被改动。
 *
 * 识别的命令（与 HTTP API 同义）：
 *   rojo|red / alerta|yellow / verde|green / off
 *   status
 *   task <文本…>            （文本可含空格；空文本清空摘要）
 *   decision [秒]           （缺省 60，钳在 1..120）
 *   link wifi|ble           （切换链路模式，重启生效；缺省=读当前值）
 */
bool lc_exec(const lc_host_t *h, const char *cmd, char *out, size_t osz);

/* 决定名（"allow"/"deny"/…），给回执和日志共用 */
const char *lc_dec_name(lc_dec_t d);
