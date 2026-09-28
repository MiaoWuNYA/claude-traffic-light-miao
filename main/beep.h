/* beep.h — 小喵掌机无源蜂鸣器（GPIO14，LEDC PWM）
 *
 * 硬件依据：学而思小喵掌机 GPIO14 无源蜂鸣器，与 tetris-miao 的
 * docs/spec/firmware/buzzer.md 一致（那边用 LEDC ch0、duty 96/255）。
 *
 * 播放是**异步**的：beep_play() 只登记一段音序就返回，真正的推进在主循环里
 * 由 beep_tick() 完成。这样 HTTP / BLE 的任务上下文里可以安全地触发声音，
 * 不需要在那里 sleep，也不会和 LVGL 抢线程。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 初始化 LEDC（QEMU 变体下是空操作，只打日志）*/
void beep_init(void);

/* 主循环里调用，推进当前音序。不播放时开销是一次比较。*/
void beep_tick(void);

/* 异步播放一段音序：freqs[i]==0 表示静音（用来做两声之间的间隔）。
 * 关掉声音开关时直接忽略。n 上限 8。*/
void beep_play(const uint32_t *freqs, const uint16_t *durs, int n);

/* 「滴 滴」两声——任务完成 / 需要操作时的提示音 */
void beep_two(void);

/* 立刻停声并清空音序 */
void beep_stop(void);

/* 声音开关（存 NVS，掉电保留）*/
bool beep_enabled(void);
void beep_set_enabled(bool on);
void beep_load(void);
