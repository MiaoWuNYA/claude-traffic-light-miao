/*
 * mochi.h — cc-mochi 表情脸移植（160×128 LVGL canvas 版）
 *
 * 移植自 https://github.com/alvis-HaoH/cc-mochi （Arduino/Adafruit_GFX 240×240），
 * 按小喵掌机 160×128 重排布局。原项目的 Claude 风格脸：
 * 暖橙底 + 深色圆眼（会眨眼、会脸红）+ 弧线嘴 + 状态小道具。
 *
 * 状态映射：
 *   MO_OFF    睡觉（闭眼弧 + zZ）
 *   MO_RED    思考（圆眼 + 瞳孔上移 + 思考泡泡）
 *   MO_YELLOW 等授权（圆眼 + O 嘴 + 闪烁 ! 框）
 *   MO_GREEN  成功（∩∩ 眼 + 大微笑 + 闪烁星星）
 */
#pragma once

#include <stdbool.h>

typedef enum {
    MO_OFF = 0,
    MO_RED,
    MO_YELLOW,
    MO_GREEN,
} mochi_state_t;

/* 在主屏 parent（160×128）上创建全屏表情 canvas，然后所有 label 叠加在它上面 */
void mochi_build(void *parent);

/* 状态切换时整脸重绘 */
void mochi_draw(mochi_state_t st);

/* 主循环里每次迭代调用：内部 90ms 节流，驱动呼吸/眨眼/瞳孔/道具动画；
 * 不在主屏时自动跳过 */
void mochi_tick(void);
