/* ble_link.h — 蓝牙（BLE）链路：掌机作为 GATT 外设等主机来连
 *
 * 和 HTTP 那条路是**同一个命令集**（rojo/alerta/verde/off/status/task/decision），
 * 语义由 link_cmd.c 保证一致。哪条路在用由掌机上的「设置 → 连接」决定。
 */
#pragma once

#include <stdbool.h>

#include "link_cmd.h"

/* 起 NimBLE 外设：注册 GATT 服务 + 开始广播。
 * h 指向的 lc_host_t 必须活到进程结束（调用方给静态存储）。*/
void ble_link_start(const lc_host_t *h);

/* 主动给已连接的客户端推一条通知。没连上就丢弃（返回 false）。*/
bool ble_link_notify(const char *txt);

/* 当前有主机连着吗 */
bool ble_link_connected(void);
