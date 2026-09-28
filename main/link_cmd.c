/* link_cmd.c — 见 link_cmd.h。纯 C，不依赖 ESP-IDF，可 host 单测。 */
#include "link_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lc_dec_name(lc_dec_t d) {
    switch (d) {
    case LC_DEC_ALLOW:  return "allow";
    case LC_DEC_DENY:   return "deny";
    case LC_DEC_ALWAYS: return "always";
    case LC_DEC_RETRY:  return "retry";
    default:            return "none";
    }
}

static bool ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 跳过空白，切出第一个词（原地写 '\0'），*rest 指向其后的参数 */
static char *split_word(char *s, char **rest) {
    while (*s == ' ' || *s == '\t') s++;
    char *w = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = '\0';
    while (*s == ' ' || *s == '\t') s++;
    *rest = s;
    return w;
}

bool lc_exec(const lc_host_t *h, const char *cmd, char *out, size_t osz) {
    if (!h || !cmd || !out || osz == 0) return false;

    /* 拷进本地缓冲再切词，不动调用方的字符串。
     * BLE 单包就这么大，128 足够（HTTP 那条路不走这里）。*/
    char buf[128];
    /* 去掉尾部空白（\r\n 和空格），否则摘要末尾会挂一串空格 */
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r' ||
                     cmd[n - 1] == ' '  || cmd[n - 1] == '\t')) n--;
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, cmd, n);
    buf[n] = '\0';

    char *rest = NULL;
    char *verb = split_word(buf, &rest);
    if (!*verb) return false;

    if (ieq(verb, "rojo") || ieq(verb, "red")) {
        if (h->set_light) h->set_light(LC_RED);
        snprintf(out, osz, "ok rojo\n");
    } else if (ieq(verb, "alerta") || ieq(verb, "yellow")) {
        if (h->set_light) h->set_light(LC_YELLOW);
        snprintf(out, osz, "ok alerta\n");
    } else if (ieq(verb, "verde") || ieq(verb, "green")) {
        if (h->set_light) h->set_light(LC_GREEN);
        snprintf(out, osz, "ok verde\n");
    } else if (ieq(verb, "off")) {
        if (h->set_light) h->set_light(LC_OFF);
        snprintf(out, osz, "ok off\n");
    } else if (ieq(verb, "status")) {
        const char *s = h->status ? h->status() : NULL;
        snprintf(out, osz, "state: %s\n", s ? s : "?");
    } else if (ieq(verb, "task")) {
        if (h->set_task) h->set_task(rest);
        snprintf(out, osz, "ok task\n");
    } else if (ieq(verb, "decision")) {
        int t = LC_DEC_TIMEOUT_DEFAULT;
        if (*rest) {
            t = atoi(rest);
            if (t < 1) t = 1;
            if (t > LC_DEC_TIMEOUT_MAX) t = LC_DEC_TIMEOUT_MAX;
        }
        lc_dec_t d = h->wait_decision ? h->wait_decision(t) : LC_DEC_NONE;
        snprintf(out, osz, "%s\n", lc_dec_name(d));
    } else if (ieq(verb, "link")) {
        if (h->switch_link) snprintf(out, osz, "%s\n", h->switch_link(rest));
        else snprintf(out, osz, "err unsupported\n");
    } else {
        return false;   /* 不认识的命令：out 不动，调用方自己回错误 */
    }
    return true;
}
