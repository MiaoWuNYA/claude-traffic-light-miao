/*
 * mochi.c — cc-mochi 表情脸移植到 160×128
 *
 * 原项目把脸画在 Adafruit_GFX 上整屏重绘；这里改成单张全屏 canvas，
 * 每次动画帧先清画布再重画（LVGL 会只把脏区推到 SPI，160×128 全帧
 * 也只要 ~40KB SPI 传输，60MHz 下 ~6ms，可以接受）。
 *
 * 绘图基元（对应 cc-mochi.ino 的 tft.fill* / drawThickLine / drawArcLine）：
 *   px / fill_rect / fill_circle / fill_round_rect / thick_line / arc_line
 */
#include "mochi.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

/* ── 配色（取自 cc-mochi initColours / drawClaudeBlush）─────────────── */
#define M_ORANGE   0xC73500     /* 暖橙底（Claude 主题）*/
#define M_FACE     0x14161A     /* 深色脸（眼/嘴）*/
#define M_BLUSH    0xC46849     /* 腮红 */
#define M_BLUSH_HI 0xFF6B35     /* success 亮腮红 */
#define M_BUBBLE   0xFFE3D3     /* 思考泡泡 */
#define M_BUBBLE_D 0xC98D76     /* 泡泡里暗点 */
#define M_STAR     0xF6D34A     /* 成功星星 */
#define M_WARN     0xF6D34A     /* ! 警告框 */
#define M_ZZZ      0xFFF3B0     /* 睡觉 zZ */

#define W 160
#define H 128

/* ── 画布与布局 ─────────────────────────────────────────────────────── */
static lv_obj_t *s_canvas;
/* LVGL 9 的 lv_color_t 是 3 字节的 {b,g,r} 结构，而 canvas 的
 * LV_COLOR_FORMAT_RGB565 是 2 字节/像素。若用 lv_color_t* 索引这块缓冲区，
 * 写入按 3 字节步进、LVGL 按 2 字节读取 → 整个画面变成周期 3 的摩尔纹（花屏）。
 * 所以这里必须用 uint16_t，写入前经 lv_color_to_u16() 打包成 RGB565。 */
static uint16_t *s_buf;                 /* W*H 全帧，RGB565 原始像素 */
static mochi_state_t s_st = MO_OFF;

/* 原版 240 高，我们在 128 高上压缩：眼 baseY 100→52，嘴 168→88 */
#define BASE_Y      52                  /* 眼中心基线（呼吸偏移前）*/
#define MOUTH_Y     88                  /* 嘴基线 */
#define BLUSH_Y     80                  /* 腮红基线 */
#define EYE_LX      38                  /* 左眼 x（右眼 = W-EYE_LX-EYE_W）*/
#define EYE_W       26
#define EYE_H       38

/* ── 动画状态（对应 StatusAnim）────────────────────────────────────── */
static uint32_t s_state_enter;          /* 当前状态开始时刻 */
static uint32_t s_last_tick;
static uint32_t s_last_blink;
static uint32_t s_blink_gap = 3000;
static bool     s_blinking;
static int8_t   s_breath;               /* -2..2 */
static int8_t   s_pdx, s_pdy;           /* 瞳孔偏移 */

/* ── 基元 ──────────────────────────────────────────────────────────── */
static inline uint16_t px(int x, int y) { return y * W + x; }

static void fill_rect(int x, int y, int w, int h, lv_color_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) s_buf[px(i, j)] = lv_color_to_u16(c);
}

static void fill_circle(int cx, int cy, int r, lv_color_t c) {
    for (int j = cy - r; j <= cy + r; j++)
        for (int i = cx - r; i <= cx + r; i++) {
            int dx = i - cx, dy = j - cy;
            if (dx * dx + dy * dy <= r * r && i >= 0 && i < W && j >= 0 && j < H)
                s_buf[px(i, j)] = lv_color_to_u16(c);
        }
}

static void fill_round_rect(int x, int y, int w, int h, int r, lv_color_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill_rect(x + r, y, w - 2 * r, h, c);
    fill_rect(x, y + r, w, h - 2 * r, c);
    fill_circle(x + r, y + r, r, c);
    fill_circle(x + w - r - 1, y + r, r, c);
    fill_circle(x + r, y + h - r - 1, r, c);
    fill_circle(x + w - r - 1, y + h - r - 1, r, c);
}

static void thick_line(int x0, int y0, int x1, int y1, int thk, lv_color_t c) {
    /* 简易 Bresenham，thk=0 画 1px，正数加十字加粗（对应原版 drawThickLine）*/
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int i = -thk; i <= thk; i++) {
            int a = x0 + i, b = y0 + i;
            if (a >= 0 && a < W && y0 >= 0 && y0 < H) s_buf[px(a, y0)] = lv_color_to_u16(c);
            if (x0 >= 0 && x0 < W && b >= 0 && b < H) s_buf[px(x0, b)] = lv_color_to_u16(c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 对应原版 drawArcLine：deg 为屏幕坐标角度（y 向下），step 6° */
static void arc_line(int cx, int cy, int r, int a0, int a1, int thk, lv_color_t c) {
    int step = a0 <= a1 ? 6 : -6;
    int px_ = cx + (int)(cosf(a0 * 0.0174533f) * r);
    int py_ = cy + (int)(sinf(a0 * 0.0174533f) * r);
    for (int d = a0 + step; step > 0 ? d <= a1 : d >= a1; d += step) {
        float rad = d * 0.0174533f;
        int nx = cx + (int)(cosf(rad) * r);
        int ny = cy + (int)(sinf(rad) * r);
        thick_line(px_, py_, nx, ny, thk, c);
        px_ = nx; py_ = ny;
    }
}

/* ── cc-mochi 状态小道具（缩到 160 宽）─────────────────────────────── */
static void prop_thinking(void) {
    uint8_t lit = (lv_tick_get() / 320) % 3;
    fill_circle(120, 26, 7, lv_color_hex(M_BUBBLE));
    for (int i = 0; i < 3; i++)
        fill_circle(112 + i * 5, 34 + i * 3, 2, lv_color_hex(M_BUBBLE));
    for (int i = 0; i < 3; i++)
        fill_circle(114 + i * 4, 25, 1, lv_color_hex(i == lit ? M_FACE : M_BUBBLE_D));
}

static void prop_permission(void) {
    if ((lv_tick_get() / 500) % 2 == 0) {
        fill_round_rect(66, 98, 28, 26, 6, lv_color_hex(M_WARN));
        /* 手画 "!"：竖条 + 点 */
        fill_rect(78, 102, 4, 10, lv_color_hex(M_FACE));
        fill_rect(78, 116, 4, 4, lv_color_hex(M_FACE));
    }
}

static void draw_pixel_star(int x, int y, lv_color_t c) {
    fill_rect(x + 2, y, 3, 7, c);
    fill_rect(x, y + 2, 7, 3, c);
}

static void prop_success(void) {
    if ((lv_tick_get() / 320) % 2 == 0) draw_pixel_star(22, 24, lv_color_hex(M_STAR));
    else                               draw_pixel_star(126, 20, lv_color_hex(M_STAR));
    draw_pixel_star(134, 34, lv_color_hex(M_STAR));
}

/* 画一个 Z：顶横 + 斜线 + 底横，三段共用同一个 x 范围。
 * （原来底横比顶横左移了 8px，斜线落到顶横右端、底横右端，字形是歪的）*/
static void draw_z(int x, int y, int w, int h, int thk, lv_color_t c) {
    fill_rect(x, y, w, thk, c);                          /* 顶横 */
    thick_line(x + w - 1, y, x, y + h - thk, 0, c);      /* 斜线：右上 → 左下 */
    fill_rect(x, y + h - thk, w, thk, c);                /* 底横 */
}

static void prop_sleeping(void) {
    /* 原版这里有一条 120x3 的「地影」横条，在 160x128 的屏上看着就是
     * 嘴下面突兀一条超长黑杠，删掉。 */
    /* ph 在 0/1 间交替，两个 Z 一起向左上飘，形成 zZ 冒泡的动画 */
    uint32_t ph = (lv_tick_get() / 400) % 2;
    draw_z(118 - ph * 4, 30 - ph * 6, 9, 10, 2, lv_color_hex(M_ZZZ));   /* 小 z */
    draw_z(132, 14 - ph * 4, 11, 13, 3, lv_color_hex(M_ZZZ));           /* 大 Z */
}

/* ── Claude 脸（对应 drawClaudeExpression/Mouth/Blush，布局压缩）────── */
static void draw_face(mochi_state_t st) {
    lv_color_t face = lv_color_hex(M_FACE);
    fill_rect(0, 0, W, H, lv_color_hex(M_ORANGE));

    int by = BASE_Y + s_breath;
    int rx = W - EYE_LX - EYE_W;                     /* 右眼 x */

    if (st == MO_OFF) {                              /* 睡觉：闭眼弧 + zZ */
        arc_line(EYE_LX + EYE_W / 2, by + 12, 11, 200, 340, 1, face);
        arc_line(rx + EYE_W / 2, by + 12, 11, 200, 340, 1, face);
        /* 睡觉嘴：r=5/thk=0 的"小平弧"实际画出来只有两个孤立像素点，
         * 看着像嘴下面多了个"--"。改成向下弯的浅笑弧（弧心在上），
         * 宽 20px、thk=1，和闭眼弧一个画法，看清是嘴。 */
        arc_line(W / 2, 74, 12, 30, 150, 1, face);
        prop_sleeping();
        return;
    }
    if (st == MO_GREEN) {                            /* 成功：∩∩ 眼 + 大笑 */
        arc_line(EYE_LX + EYE_W / 2, by + 12, 12, 205, 335, 1, face);
        arc_line(rx + EYE_W / 2, by + 12, 12, 205, 335, 1, face);
        arc_line(W / 2, MOUTH_Y - 8, 13, 25, 155, 2, face);
        prop_success();
        goto blush;
    }

    /* 通用圆眼（眨眼时压成 4px 横条）*/
    {
        int h = s_blinking ? 4 : EYE_H;
        int ytop = by + (EYE_H - h) / 2;
        int r = h / 2 < 8 ? h / 2 : 8;
        int tilt = (st == MO_RED) ? 4 : 0;           /* thinking 头微歪 */
        fill_round_rect(EYE_LX, ytop, EYE_W, h, r, face);
        fill_round_rect(rx, ytop + tilt, EYE_W, h, r, face);
        if (!s_blinking) {
            fill_circle(EYE_LX + EYE_W / 2 + s_pdx, ytop + h / 2 + 3 + s_pdy, 4,
                        lv_color_hex(M_ORANGE));
            fill_circle(rx + EYE_W / 2 + s_pdx, ytop + tilt + h / 2 + 3 + s_pdy, 4,
                        lv_color_hex(M_ORANGE));
        }
    }

    /* 嘴（对应 drawClaudeMouth）*/
    if (st == MO_RED)         arc_line(W / 2, MOUTH_Y - 2, 7, 205, 335, 1, face);  /* 沉思波浪 */
    else if (st == MO_YELLOW) {                                                              /* O 嘴 */
        for (int d = 0; d < 360; d += 6) {
            float rad = d * 0.0174533f;
            int a = W / 2 + (int)(cosf(rad) * 5);
            int b = MOUTH_Y + (int)(sinf(rad) * 5);
            fill_circle(a, b, 1, face);
        }
    }

blush:                                                /* 腮红（error 类状态无，这里只有黄有点淡腮红）*/
    if (st != MO_OFF) {
        lv_color_t bc = lv_color_hex(st == MO_GREEN ? M_BLUSH_HI : M_BLUSH);
        fill_circle(26, BLUSH_Y + s_breath, st == MO_GREEN ? 7 : 6, bc);
        fill_circle(W - 26, BLUSH_Y + s_breath, st == MO_GREEN ? 7 : 6, bc);
    }

    if (st == MO_RED)     prop_thinking();
    else if (st == MO_YELLOW) prop_permission();
}

/* ── 对外接口 ──────────────────────────────────────────────────────── */
static void flush_canvas(void) {
    lv_obj_invalidate(s_canvas);
}

void mochi_build(void *parent) {
    s_canvas = lv_canvas_create((lv_obj_t *)parent);
    /* 全帧 40KB 不能走 lv_malloc（LVGL 内置堆只有 64KB，会申请失败返回 NULL），
     * 用系统堆；WiFi 版有 PSRAM，QEMU 版落到内部 RAM 也能放下 */
    s_buf = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_8BIT);
    ESP_LOGI("mochi", "build: buf=%p", s_buf);
    if (!s_buf) { lv_obj_delete(s_canvas); s_canvas = NULL; return; }
    lv_canvas_set_buffer(s_canvas, s_buf, W, H, LV_COLOR_FORMAT_RGB565);
    ESP_LOGI("mochi", "build: set_buffer done");
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_move_background(s_canvas);
    s_state_enter = lv_tick_get();
    s_last_blink = s_state_enter;
    draw_face(MO_OFF);
    lv_obj_invalidate(s_canvas);
}

void mochi_draw(mochi_state_t st) {
    s_st = st;
    s_state_enter = lv_tick_get();
    s_last_blink = s_state_enter;
    draw_face(st);
    flush_canvas();
}

void mochi_tick(void) {
    if (!s_canvas) return;
    if ((lv_obj_t *)lv_screen_active() != lv_obj_get_screen(s_canvas)) return;
    if (s_st == MO_OFF) return;                      /* 睡觉脸是静态的，省电 */

    uint32_t now = lv_tick_get();
    if (now - s_last_tick < 90) return;
    s_last_tick = now;

    /* 呼吸三角波 -2..2（周期按状态：busy 3.5s，success 3.6s）*/
    uint32_t period = 3500;
    if (s_st == MO_GREEN) period = 3600;
    float bp = (float)(now % period) / period;
    float tri = 1.0f - fabsf(bp * 2.0f - 1.0f);
    s_breath = (int8_t)lroundf((tri - 0.5f) * 4.0f);

    /* 随机眨眼 */
    s_blinking = false;
    if (now - s_last_blink > s_blink_gap) {
        s_last_blink = now;
        s_blink_gap = 1800 + rand() % 3200;
        if (rand() % 100 < 15) s_blink_gap = 220;
    }
    s_blinking = (now - s_last_blink) < 120;

    /* 瞳孔漂移 */
    s_pdx = 0; s_pdy = 0;
    if (s_st == MO_RED)        s_pdy = -3;
    else if (s_st == MO_YELLOW) s_pdx = (int8_t)lroundf(sinf(now / 520.0f) * 4);

    draw_face(s_st);
    flush_canvas();
}
