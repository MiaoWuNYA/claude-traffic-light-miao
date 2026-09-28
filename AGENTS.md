# AGENTS.md — 让任意 coding agent 使用小喵掌机状态灯

> 这份文档面向**任何** agent（Claude Code、Codex CLI、Gemini CLI、自研 agent …）。
> 讲清楚三件事：掌机是什么、怎么连上它、怎么把状态/授权接进你自己的 agent 循环。
> Claude Code 专属的钩子配置在最后（§6），其它 agent 可以直接跳过。

## 1. 这是什么

一台 ESP32 掌机（160×128 彩屏 + 蜂鸣器 + 6 个按键），显示 agent 的当前状态：

| 颜色 | 屏幕 | 含义 |
|------|------|------|
| 红 | `CLAUDE BUSY` | agent 开始干活（你提交请求时发）|
| 黄（闪烁）| `NEEDS YOU !` | agent 需要用户确认（会滴两声）|
| 绿 | `TASK DONE` | 任务完成（会滴两声）|
| 灭 | `OFF` | 空闲 |

屏幕底部一行还能显示**当前任务摘要**（你发命令时顺带把任务文本推过来），
右上角是这段任务跑了多少秒，绿灯亮起停表。

两种连电脑的方式，**命令集完全一样**，在掌机的设置菜单里二选一：

- **WiFi**：掌机连你的局域网，起一个 HTTP 服务（端口 80）
- **蓝牙（BLE）**：掌机作为 GATT 外设广播，名字 `TLMIAO`，不需要 IP

## 2. 找到设备

### WiFi（推荐）

向局域网广播一个 UDP 包，设备自己报 IP：

```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.settimeout(0.8)
s.sendto(b"TLMIAO?", ("255.255.255.255", 4210))
data, _ = s.recvfrom(64)          # → b"TLMIAO 192.168.1.178"
ip = data.split()[1].decode()
```

- 只有设备**开着 WiFi 模式且已连上网**才会应答；蓝牙模式下先走 §5 切回来。
- 建议把找到的 IP 缓存下来，连不上再重新广播（别每条命令都广播）。

### 蓝牙

设备名 `TLMIAO`，一个服务两个特征：

```
服务  6d69616f-0001-4000-8000-00805f9b34fb
RX    6d69616f-0002-4000-8000-00805f9b34fb   ← 写命令进来
TX    6d69616f-0003-4000-8000-00805f9b34fb   → 回执走 notify
```

用 bleak（Python）的完整往返——**先订阅 TX 再写 RX**，否则回执会漏：

```python
import asyncio
from bleak import BleakScanner, BleakClient

SVC = "6d69616f-0001-4000-8000-00805f9b34fb"
RX  = "6d69616f-0002-4000-8000-00805f9b34fb"
TX  = "6d69616f-0003-4000-8000-00805f9b34fb"

async def ble_cmd(line: str, timeout=10.0) -> str:
    dev = await BleakScanner.find_device_by_name("TLMIAO", timeout=4.0)
    if dev is None:
        raise RuntimeError("没扫到 TLMIAO（掌机切到蓝牙模式了吗？）")
    loop = asyncio.get_running_loop()
    fut = loop.create_future()
    def on_notify(_c, data):
        if not fut.done(): fut.set_result(bytes(data))
    async with BleakClient(dev, timeout=8.0) as cli:
        await cli.start_notify(TX, on_notify)
        await cli.write_gatt_char(RX, line.encode("utf-8"), response=True)
        reply = await asyncio.wait_for(fut, timeout)
    return reply.decode("utf-8", "replace").strip()

print(asyncio.run(ble_cmd("status")))   # → "state: verde"
```

- BLE 连一次要一两秒，**不要卡在用户输入的关键路径上**——状态推送最好
  丢到后台线程/子进程发。
- 每条命令一行（`\n` 结尾可有可无），一包不超过 128 字节。
- 本机已有的实现可直接抄：`test/fake_ble.py`（还兼做桌面端模拟器）。

## 3. 命令集（两种链路通用）

HTTP 端是路径 + 可选查询参数；BLE 端是往 RX 特征写一行文本。语义一比一：

| 作用 | HTTP | BLE 命令 | 成功回执 |
|------|------|----------|----------|
| 红（开始干活）| `GET/POST /solo/rojo` | `rojo`（别名 `red`）| `ok rojo` |
| 黄（需要用户）| `GET/POST /alerta` | `alerta`（别名 `yellow`）| `ok alerta` |
| 绿（任务完成）| `GET/POST /solo/verde` | `verde`（别名 `green`）| `ok verde` |
| 熄灭 | `GET/POST /off` | `off` | `ok off` |
| 查状态 | `GET /status` | `status` | `state: verde\nip: 192.168.1.178\nclk: 60` |
| 推任务摘要 | `GET /task?text=<URL编码>` 或 `POST /task`（正文即文本）| `task <文本>` | `ok task` |
| 等授权决定 | `GET /decision?timeout=N` | `decision [N]` | `allow` / `deny` / `always` / `retry` / `none` |
| 链路查询/切换 | `GET /link`、`GET /link?mode=wifi\|ble` | `link [wifi\|ble]` | `link: wifi` / `ok link, restart in 1s` |

细节和坑：

- **`/task` 的文本最长 30 字节**（固件截断），CJK 会占多点，超长会被截。
- 推摘要只在 `rojo`/`red`（开始干活）时有意义——设备把它显示在底部一行。
- **黄灯 30 秒无人理会自动熄灭**，但 `/decision` 挂着等按键期间不会灭。
- **`/decision` 会占住设备整个 HTTP 服务**（单 task 长轮询），等待期间其它
  HTTP 请求排队。它同时把灯打黄、蜂鸣器滴两声。`N` 上限 120 秒，缺省 60。
- `link <wifi|ble>` 切换后**设备 1 秒内重启**：WiFi→蓝牙会失去 IP，
  蓝牙→WiFi 要等它连上网。建议只在用户明确要求时用。
- 不认识的命令：HTTP 返回 `err: ...`，BLE 回 `err unknown`。
- 全部命令**无鉴权**：WiFi 模式下只有你局域网里的人能发；蓝牙模式是
  附近任何人都能连（GATT 不配对）。别把敏感信息推到 `/task`。

### HTTP 速查（curl）

```bash
IP=192.168.1.178
curl "http://$IP/status"
curl "http://$IP/solo/rojo"
curl "http://$IP/task?text=修登录bug"
curl "http://$IP/decision?timeout=60"    # 阻塞最多 60s，等用户在掌机上按方向键
curl "http://$IP/link"
```

### 决定（decision）的语义

设备端 `decision` 命令：灯转黄 + 滴两声 → 用户在掌机上按方向键 → 返回：

| 用户按键 | 返回值 | 建议的 agent 行为 |
|---------|--------|------------------|
| ↑ | `allow` | 这次操作放行 |
| ← | `always` | 放行，并把这个工具写进持久白名单 |
| ↓ | `retry` | 拒绝**并换个做法重试**（用户按的是「重来」）|
| → | `deny` | 拒绝这次操作 |
| 超时没按 | `none` | 交回你自己的授权流程（弹终端询问等）|

`retry` 和 `deny` 在语义上的区别要传给模型：retry 是「换个办法再来一次」，
deny 是「别做了」。

## 4. 推荐的接入方式（任意 agent）

把你 agent 的生命周期事件映射到命令：

| agent 事件 | 发什么 |
|-----------|--------|
| 用户提交任务、agent 开始工作 | `rojo` + `task <任务摘要>` |
| agent 需要用户确认工具调用 | ① `alerta`（快速提示）或 ② `decision`（接管确认）|
| agent 回答完成 | `verde` |
| 会话开始/结束 | `off` |

实现要点（这套已经在一台真实设备上验证过）：

1. **状态推送绝不能阻塞主循环**。HTTP 下把命令丢后台子进程/线程发；
   BLE 下更要如此（连接本身要 1–2 秒）。
2. **失败要无声**——灯只是个外设，推状态失败不应该打断 agent 工作流。
   想排查就手动跑（见 §3 速查）看哪步出错。
3. **IP/链路要缓存**，连不上再重新发现，并做限流（建议 15 秒内不重复广播）。
4. `/decision` 用的时候给个比设备侧 timeout 更长的超时（+10 秒），
   设备侧没拿到就当 `none` 处理。
5. 一台现成的参考实现：`~/.claude/hooks/traffic-light.py`（纯 Python 标准库，
   BLE 路径依赖 bleak，装在 `~/.local/share/traffic-light-miao/pylib`）。
   它支持 `rojo/alerta/verde/off/status/task/ask` 子命令，两条链路自动择优
   并缓存到 `~/.cache/traffic-light-miao/`。

## 5. 掌机端怎么配

主界面按键：**短按 ↑** 进设置菜单（↑↓ 移光标、A 改、B 退），
**长按 ↑** 直接进 WiFi 重扫。

| 设置项 | 值 | 说明 |
|--------|-----|------|
| 显示 IP | 开/关 | 底部一行常显 IP 还是显示任务摘要 |
| 声音 | 开/关 | 黄灯/绿灯时是否滴两声 |
| 连接 | WiFi/蓝牙 | **改完提示 RESTART，手动重启生效**（或发 `link` 命令）|
| 重设 WiFi | — | 重扫热点，回到 WiFi 配置界面 |

WiFi 配置在掌机屏上完成（选热点、软键盘输密码），连过一次存 NVS，
下次开机自动连。蓝牙模式不需要任何配置。

## 6. Claude Code 专属配置（其它 agent 可忽略）

`~/.claude/settings.json` 的 hooks（`traffic-light.py` 需在 `~/.claude/hooks/`）：

```json
{
  "hooks": {
    "UserPromptSubmit":    [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py rojo", "timeout": 5 }] }],
    "Notification":        [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py alerta", "timeout": 5 }] }],
    "Stop":                [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py verde", "timeout": 5 }] }],
    "SessionStart":        [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py off", "timeout": 5 }] }],
    "SessionEnd":          [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py off", "timeout": 5 }] }],
    "PermissionRequest":   [{ "hooks": [{ "type": "command",
        "command": "/home/<user>/.claude/hooks/traffic-light.py ask", "timeout": 90 }] }]
  }
}
```

- `UserPromptSubmit` 的钩子**会把 stdout 注入模型上下文**，所以脚本默认静默
  （`-v` 才往 stderr 打日志）。用户输入的任务摘要由脚本自己从 stdin 的
  钩子 JSON 里读出来推给 `/task`。
- `PermissionRequest` 挂上后，**每次工具授权会阻塞到你在掌机上按方向键或
  超时**（默认 60 秒，环境变量 `TRAFFIC_LIGHT_ASK_TIMEOUT` 可调；掌机钩子的
  settings timeout 是 90 秒）。脚本输出 `hookSpecificOutput.decision` 结构，
  掌机按 ← 时会顺手把该工具写进**当前项目**的 `.claude/settings.local.json`。
- 想临时停用：删掉 settings.json 里的 `PermissionRequest` 段（或者整个 hooks）。

## 7. 故障排查

| 现象 | 排查 |
|------|------|
| WiFi 发现不到设备 | 掌机是否 WiFi 模式且已连上网（屏上有 `http://<ip>`）？同网段？手动 `curl http://<ip>/status` |
| 蓝牙扫不到 `TLMIAO` | 掌机是否已切蓝牙模式并重启？本机蓝牙适配器是否开启？ |
| `state` 一直是 `off` | 正常——off 是空闲态。发 `rojo` 看灯有没有反应 |
| 黄灯亮了又自己灭了 | 30 秒无人理会自动熄灭（有 `decision` 挂着时不会）|
| `link` 切换后连不上了 | 正常——切换要重启。等 2 秒，蓝牙模式重新扫描，WiFi 模式重新发现 |
| 掌机 IP 变了 | 删掉你的缓存重新广播发现 |

## 8. 构建与烧录（想改固件时）

见 `README.md`。简版：`./build.sh`（WiFi 版）或 `./build.sh qemu`（无硬件逻辑验证）；
真机只烧 app 分区 `esptool write_flash 0x10000 build/traffic-light-miao.bin`。
命令语义层（`main/link_cmd.c`）有主机单测：`make -C test check`。
