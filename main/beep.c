/* beep.c — 见 beep.h */
#include "beep.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "sdkconfig.h"

/* QEMU 的 esp32 机型没有 LEDC 外设模型（和没有 WiFi 基带一样），
 * 真机上才真的去驱动蜂鸣器。本项目的 QEMU 变体就是
 * CONFIG_TRAFFIC_ENABLE_WIFI=n 那一份，沿用同一个开关。 */
#if CONFIG_TRAFFIC_ENABLE_WIFI
#define BEEP_HW 1
#else
#define BEEP_HW 0
#endif

#if BEEP_HW
#include "driver/ledc.h"
#endif

static const char *TAG = "beep";

#define BEEP_GPIO   14        /* 无源蜂鸣器（学而思小喵掌机 / tetris-miao 一致）*/
#define BEEP_DUTY   96        /* /255，与 tetris-miao 相同 */
#define BEEP_MAX    8         /* 一段音序最多几个音 */

#define CFG_NS      "tlcfg"   /* 与 WiFi 凭据同一个 NVS 命名空间 */
#define KEY_SOUND   "sound"

static bool     s_on = true;
static uint32_t s_freq[BEEP_MAX];
static uint16_t s_dur[BEEP_MAX];
static int      s_n;          /* 本段音序长度 */
static int      s_i;          /* 当前播到第几个 */
static uint32_t s_step_end;   /* 当前音的结束时刻（ms）*/
static bool     s_active;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void hw_tone(uint32_t freq) {
#if BEEP_HW
    if (freq == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    } else {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, BEEP_DUTY);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    ESP_LOGI(TAG, "tone %u Hz", (unsigned)freq);
#endif
}

void beep_init(void) {
#if BEEP_HW
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 2000,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t ch = {
        .gpio_num = BEEP_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    ESP_LOGI(TAG, "buzzer ready (GPIO%d, LEDC ch0)", BEEP_GPIO);
#else
    ESP_LOGI(TAG, "QEMU: 蜂鸣器不驱动，只打日志");
#endif
}

void beep_stop(void) {
    s_active = false;
    s_n = s_i = 0;
    hw_tone(0);
}

void beep_play(const uint32_t *freqs, const uint16_t *durs, int n) {
    if (!s_on || !freqs || !durs || n <= 0) return;
    if (n > BEEP_MAX) n = BEEP_MAX;
    memcpy(s_freq, freqs, (size_t)n * sizeof s_freq[0]);
    memcpy(s_dur, durs, (size_t)n * sizeof s_dur[0]);
    s_n = n;
    s_i = 0;
    s_active = true;
    hw_tone(s_freq[0]);
    s_step_end = now_ms() + s_dur[0];
}

void beep_tick(void) {
    if (!s_active) return;
    if ((int32_t)(now_ms() - s_step_end) < 0) return;   /* 这一声还没放完 */
    if (++s_i >= s_n) { beep_stop(); return; }
    hw_tone(s_freq[s_i]);
    s_step_end = now_ms() + s_dur[s_i];
}

void beep_two(void) {
    /* 滴 滴：两声短促的同音，中间留一点间隔才听得出是「两声」*/
    static const uint32_t f[] = { 2200, 0, 2200 };
    static const uint16_t d[] = {   70, 55,   70 };
    beep_play(f, d, 3);
}

bool beep_enabled(void) { return s_on; }

void beep_set_enabled(bool on) {
    s_on = on;
    if (!on) beep_stop();
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_SOUND, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "sound %s", on ? "on" : "off");
}

void beep_load(void) {
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return;   /* 没存过 → 默认开 */
    uint8_t v = 1;
    if (nvs_get_u8(h, KEY_SOUND, &v) == ESP_OK) s_on = v ? true : false;
    nvs_close(h);
}
