/* test_link_cmd.c — link_cmd 的 host 单测（不需要 ESP-IDF，也不需要硬件）
 *
 * 为什么要有这个：QEMU 的 esp32 机型没有蓝牙外设模型，BLE 那条路根本没法仿真；
 * 而 BLE 上跑的就是这套命令语义。所以把语义抽成纯函数在这里测，
 * 剩下真正没测到的就只有「NimBLE 有没有把字节送到这个函数」这一段。
 *
 * 跑法：make -C test  （或见文件末尾注释）
 */
#include <stdio.h>
#include <string.h>

#include "../main/link_cmd.h"

static int fails;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            fails++;                                                      \
        }                                                                 \
    } while (0)

#define CHECK_STR(got, want)                                              \
    do {                                                                  \
        if (strcmp((got), (want)) != 0) {                                 \
            printf("  ✗ %s:%d  期望 \"%s\"，实到 \"%s\"\n",               \
                   __FILE__, __LINE__, (want), (got));                    \
            fails++;                                                      \
        }                                                                 \
    } while (0)

/* ── 假宿主：把动作记下来 ──────────────────────────────────────────── */
static lc_light_t g_light;
static int         g_light_n;
static char        g_task[128];
static int         g_task_n;
static int         g_dec_timeout = -1;
static lc_dec_t    g_dec_reply   = LC_DEC_NONE;

static void host_set_light(lc_light_t st) { g_light = st; g_light_n++; }
static void host_set_task(const char *txt) { snprintf(g_task, sizeof g_task, "%s", txt); g_task_n++; }
static lc_dec_t host_wait_decision(int t) { g_dec_timeout = t; return g_dec_reply; }
static const char *host_status(void) { return "rojo"; }

static char g_link_msg[48];
static const char *host_switch_link(const char *arg) {
    snprintf(g_link_msg, sizeof g_link_msg, "switch:%s", arg[0] ? arg : "(query)");
    return g_link_msg;
}

static const lc_host_t HOST = {
    .set_light = host_set_light,
    .set_task = host_set_task,
    .wait_decision = host_wait_decision,
    .status = host_status,
    .switch_link = host_switch_link,
};

static void reset(void) {
    g_light = LC_OFF;
    g_light_n = g_task_n = 0;
    g_task[0] = '\0';
    g_dec_timeout = -1;
    g_dec_reply = LC_DEC_NONE;
}

static void run(const char *cmd, const char *want) {
    char out[64] = "\x01";
    bool ok = lc_exec(&HOST, cmd, out, sizeof out);
    if (!ok) {
        printf("  ✗ \"%s\" 没被识别\n", cmd);
        fails++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("  ✗ \"%s\" → \"%s\"，期望 \"%s\"\n", cmd, out, want);
        fails++;
    }
}

int main(void) {
    printf("灯状态\n");
    reset();
    run("rojo", "ok rojo\n");
    CHECK(g_light == LC_RED);
    run("red", "ok rojo\n");
    run("ALERTA", "ok alerta\n");
    CHECK(g_light == LC_YELLOW);
    run("yellow", "ok alerta\n");
    run("verde", "ok verde\n");
    CHECK(g_light == LC_GREEN);
    run("green", "ok verde\n");
    run("off", "ok off\n");
    CHECK(g_light == LC_OFF);
    CHECK(g_light_n == 7);

    printf("大小写与空白\n");
    reset();
    run("  RoJo  \n", "ok rojo\n");          /* 首尾空白 + 尾随换行 */
    run("OFF\r\n", "ok off\n");              /* CRLF（有的客户端会带）*/
    CHECK(g_light_n == 2);

    printf("status\n");
    reset();
    run("status", "state: rojo\n");

    printf("task\n");
    reset();
    run("task 修花屏并加授权候选界面", "ok task\n");
    CHECK_STR(g_task, "修花屏并加授权候选界面");
    run("task", "ok task\n");                /* 无参数 = 清空 */
    CHECK_STR(g_task, "");
    run("task   多个   空格   ", "ok task\n");
    CHECK_STR(g_task, "多个   空格");        /* 词间空格保留，尾部空白吃掉 */
    CHECK(g_task_n == 3);

    printf("decision\n");
    reset();
    g_dec_reply = LC_DEC_ALLOW;
    run("decision", "allow\n");              /* 缺省超时 */
    CHECK(g_dec_timeout == 60);
    run("decision 5", "allow\n");
    CHECK(g_dec_timeout == 5);
    g_dec_reply = LC_DEC_RETRY;
    run("decision 999", "retry\n");          /* 钳到上限 */
    CHECK(g_dec_timeout == LC_DEC_TIMEOUT_MAX);
    run("decision 0", "retry\n");            /* 钳到下限 */
    CHECK(g_dec_timeout == 1);
    run("decision -5", "retry\n");           /* 负数也钳到下限 */
    CHECK(g_dec_timeout == 1);
    g_dec_reply = LC_DEC_NONE;
    run("decision 1", "none\n");             /* 超时 */

    printf("不认识的命令\n");
    reset();
    {
        char out[64] = "\x01";
        CHECK(!lc_exec(&HOST, "reboot", out, sizeof out));
        CHECK(out[0] == '\x01');             /* 不能改动 out */
        CHECK(!lc_exec(&HOST, "", out, sizeof out));
        CHECK(!lc_exec(&HOST, "   \n", out, sizeof out));
        CHECK(!lc_exec(&HOST, "rojoo", out, sizeof out));   /* 前缀不算匹配 */
        CHECK(!lc_exec(&HOST, "ro jo", out, sizeof out));
        CHECK(!lc_exec(&HOST, "linkk", out, sizeof out));
        CHECK(g_light_n == 0 && g_task_n == 0);
    }

    printf("link 命令（查询 / 切换）\n");
    reset();
    {
        char out[64];
        CHECK(lc_exec(&HOST, "link", out, sizeof out));
        CHECK_STR(out, "switch:(query)\n");
        CHECK(lc_exec(&HOST, "link ble", out, sizeof out));
        CHECK_STR(out, "switch:ble\n");
        CHECK(lc_exec(&HOST, "link wifi", out, sizeof out));
        CHECK_STR(out, "switch:wifi\n");
        /* 参数原样传给宿主，wifi/ble 之外的值由宿主拒（这里测的是透传） */
        CHECK(lc_exec(&HOST, "link xyz", out, sizeof out));
        CHECK_STR(out, "switch:xyz\n");
    }

    printf("回调为 NULL 时不能崩\n");
    {
        static const lc_host_t empty = { 0 };
        char out[64];
        CHECK(lc_exec(&empty, "rojo", out, sizeof out));
        CHECK(lc_exec(&empty, "task x", out, sizeof out));
        CHECK(lc_exec(&empty, "decision 3", out, sizeof out));
        CHECK_STR(out, "none\n");            /* 没有 wait_decision → 当作没人按键 */
        CHECK(lc_exec(&empty, "status", out, sizeof out));
        CHECK_STR(out, "state: ?\n");
        CHECK(lc_exec(&empty, "link", out, sizeof out));
        CHECK_STR(out, "err unsupported\n");
        CHECK(!lc_exec(NULL, "rojo", out, sizeof out));
    }

    printf("超长输入不能溢出\n");
    {
        char big[600];
        memset(big, 'x', sizeof big - 1);
        big[0] = 't'; big[1] = 'a'; big[2] = 's'; big[3] = 'k'; big[4] = ' ';
        big[sizeof big - 1] = '\0';
        char out[64];
        reset();
        CHECK(lc_exec(&HOST, big, out, sizeof out));
        CHECK(strlen(g_task) == 127 - 5);    /* 本地缓冲 128，去掉 "task " */
    }

    printf("\n%s（%d 处失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
