# traffic-light-miao — Claude 状态红绿灯

> 想让**别的** agent（Codex、Gemini CLI、自研 agent…）也能用这台掌机？
> 命令接口、发现协议、接入要点都独立成文档了：**[AGENTS.md](AGENTS.md)**（agent 无关）。
> 本 README 侧重这台设备本身与 Claude Code 的配置。

小喵掌机（ESP32-WROVER-B + ST7735 160×128）上的 Claude Code 状态显示屏。
移植自 [safernandez666/vibecoding-traffic-light](https://github.com/safernandez666/vibecoding-traffic-light)，
硬件框架参照 [jsfaint/tetris-miao](https://github.com/jsfaint/tetris-miao)。

掌机屏幕显示当前 Claude 状态：

| 状态 | 屏幕显示 | 触发时机 |
|------|---------|---------|
| 🔴 红 | `CLAUDE BUSY` | UserPromptSubmit — Claude 开始工作 |
| 🟡 黄（闪烁 30s） | `NEEDS YOU !` | Notification — Claude 需要你确认 |
| 🟢 绿 | `TASK DONE` | Stop — 任务完成 |
| ⚫ 熄灭 | `OFF` | 空闲/手动关闭 |

底部一行显示**当前任务摘要**（UserPromptSubmit 时把用户输入推过来），
长按 ↑ 切到 IP 地址。右上角是**当前这段任务已经跑了多久**，和 Claude Code 终端里
显示的一致；绿灯亮起时停表，红灯重新亮起时归零重计。

任务完成或需要你操作时，**喇叭滴两声**（GPIO14 无源蜂鸣器，LEDC 驱动）。
声音开关在主界面按 **↑** 进设置菜单里改，存在 NVS。

## 设置菜单

主界面**短按 ↑** 进设置菜单，`↑↓` 移光标、`A` 改这一项、`B` 退回首页：

| 项 | 值 | 说明 |
|----|----|------|
| 显示 IP | 开 / 关 | 底部一行是否常显 IP（否则显摘要）|
| 声音 | 开 / 关 | 完成任务 / 需要你操作时是否滴两声 |
| 连接 | WiFi / 蓝牙 | 切换连电脑的链路，**改完提示 `RESTART`**，手动重启生效 |
| 重设 WiFi | — | 按 A 重扫热点，回到 WiFi 设置界面 |

每次进菜单光标都回到第一项——留在「重设 WiFi」上的话下次进来顺手一按 A
就把热点重扫了，太容易误触。

## 两种链路：WiFi 或蓝牙

掌机连电脑有两条路，命令集**完全一样**（由固件的 `main/link_cmd.c` 保证语义一致），
在设置菜单里切：

- **WiFi**：钩子先读缓存 IP，连不上就向 `255.255.255.255:4210` 广播 `TLMIAO?`，
  设备回 `TLMIAO <ip>`；命令走 HTTP :80。适合掌机和电脑在同一个局域网。
- **蓝牙（BLE）**：掌机作为 GATT 外设广播 `TLMIAO`，钩子连上去往 RX 特征写命令、
  从 TX 特征收 notify 回执。适合电脑没连热点、或想少配一个 IP 的时候。
  用 NimBLE，不配对不加密（和 HTTP 那条路一样是「谁都能发命令」的信任模型，
  但 BLE 是**附近任何人都能扫到**，不像 HTTP 只在自己局域网里）。

钩子会自动跟着走：两条都试，哪条通用哪条，并把结果缓存到
`~/.cache/traffic-light-miao/link`。推状态的命令**丢到后台子进程去发**——
蓝牙连一次要一两秒，不能卡在你按回车那条路径上。授权等待（`ask`）还是会阻塞，
那本来就是要等你按键的。

## 使用流程

开机后掌机**不写死 WiFi**，而是在屏幕上选热点：

1. **主界面按 UP** 进入 WiFi 设置，自动扫描并列出附近热点
   （`名称` + `信号强度`，带 `*` 表示加密）
2. **UP/DOWN 选热点** → **A 确认**；开放热点直接连，加密热点进密码界面
3. 密码界面：**方向键选字符** → **A 输入**；`Aa` 切大小写、`删除` 退格、`确认` 连接
4. 连上后自动回到主界面，底部显示 `http://<IP>`，并自动记住该热点
   （存在 NVS，下次开机自动连，直接跳过设置界面）

按键：

| 键 | 主界面 | 热点列表 | 密码界面 |
|----|--------|---------|---------|
| UP / DOWN | UP = 进设置 | 上下选热点 | 上下换行 |
| LEFT / RIGHT | — | — | 左右换列 |
| A | — | 选中/连接 | 输入字符 / `Aa` 切换 / `删除` / `确认` |
| B | — | 短按重扫、长按回主界面 | 短按退格、长按（空密码时）返回列表 |

密码为空时按 `确认` 会提示 `请输入密码`；连接失败会在列表上显示 `连接失败` 2.5 秒。

## 构建

```
./build.sh          # WiFi 版（真机用）→ build/traffic-light-miao-merged.bin
./build.sh qemu     # 无 WiFi 版（QEMU 验证用）→ build_qemu/…-merged.bin
```

`build.sh` 会先删掉 `sdkconfig` 再构建——两个变体共用一份 sdkconfig，不删会串配置。

真机版带 NimBLE（BLE 链路），1,658,800 字节，占 factory 分区的 **79%**，还剩 438 KB。
蓝牙只用了 NimBLE 外设那一小块；经典蓝牙 + A2DP（要 ~700 KB+）塞不进这个分区，
而且用户的分区表不能动，所以只做了 BLE。

主机单元测试（命令层 `link_cmd.c`，不需要硬件）：

```
make -C test check
```

## 烧录

真机实测分区布局（从出厂固件备份里 dump 出来的，和常见的 0x9000 布局不同）：
分区表在 **0x8000**，app 直接放 **factory (0x10000)**，没有 otadata/ota_0。
只烧 app，不动设备自己的 bootloader / 分区表 / NVS：

```
echo 密码 | sudo -S ~/.espressif/python_env/idf5.5_py3.12_env/bin/python -m esptool \
  --chip esp32 --port /dev/ttyACM0 -b 460800 \
  write_flash 0x10000 build/traffic-light-miao.bin
```

> 原厂固件（cyber_pet）的完整 4MB 备份在 `xiaomiao-original-backup.bin`，
> 想还原整机时 `write_flash 0x0 xiaomiao-original-backup.bin` 即可。

## QEMU 验证（无硬件时）

QEMU 的 esp32 机型**没有 WiFi 基带模型**（调 esp_wifi 直接 abort），而且 **SPI 传输永远不完成**
（`tx_param`/`tx_color` 会永久阻塞），所以无 WiFi 版把 LCD/SPI 整条路径绕开，屏幕内容用
字符画打印出来看。串口输入在 QEMU 里也收不到，于是内置了一条时间轴（`demo_run()`），
按真实时间自动走一遍全部界面——走的是和真机按键完全相同的 `handle_event()` 路径：

```
./qemu_test.sh      # 跑 125 秒，打印 36 张屏幕快照 + 异常行数
```

快照在 `/tmp/tl_out`：`--- N 标题 ---` 后面是控件树（位置+文字），再下面是 80×32 字符画
（` `=黑 `.`=暗 `+`=中 `#`=亮）。无 WiFi 版里按键直接驱动红绿灯状态：
**UP=进设置/黄  DOWN=熄灭  A=绿  B=红  LEFT=绿**，
设置菜单里 `↑↓` 移光标、`A` 改这一项、`B` 退出。

## Claude Code 配置

已经配好了（`~/.claude/settings.json` + `~/.claude/hooks/traffic-light.py`）：

钩子脚本在仓库里的 [`host/traffic-light.py`](host/traffic-light.py)（纯 Python 标准库，BLE 路径依赖 bleak）。

| 事件 | 灯 |
|------|----|
| UserPromptSubmit | 红（顺带把用户输入推到设备底部当摘要）|
| Notification | 黄 |
| PermissionRequest | 黄 → **等你在掌机上按键做决定**（见下）|
| Stop | 绿 |
| SessionStart / SessionEnd | 熄灭 |

钩子脚本**不写死 IP**：先读 `~/.cache/traffic-light-miao/ip`，连不上就向
`255.255.255.255:4210` 广播 `TLMIAO?`，设备回 `TLMIAO <ip>` 后写回缓存
（设备不在线时 15 秒内不重复找，保证钩子始终 ~0.2s 返回）。
掌机切到蓝牙模式时走另一条路，见上面「两种链路」。

> ⚠️ 钩子（尤其 UserPromptSubmit）的 **stdout 会被注入到上下文**，所以脚本默认一声不吭，
> 只有加 `-v` 才往 stderr 打日志。`status` 会打印，别挂到钩子上。

手动测试 / 手动点灯：

```
~/.claude/hooks/traffic-light.py -v rojo     # 红灯，并打印过程
~/.claude/hooks/traffic-light.py verde       # 绿灯
~/.claude/hooks/traffic-light.py status      # 查询当前状态
~/.claude/hooks/traffic-light.py task 改菜单 # 手动推一条任务摘要
```

换到别的机器上部署，只需拷贝 `host/traffic-light.py` 并照上表加 hooks 即可。

### 在掌机上做授权决定

Claude 需要授权（黄灯 `NEEDS YOU !`）时，**按 A 进候选界面**，方向键直接做决定：

| 键 | 屏幕 | 决定 |
|----|------|------|
| `↑` | 继续 | 允许这一次 |
| `↓` | 重来 | 拒绝，并告诉 Claude「换个做法重试」|
| `←` | 总是接受更改（绿）| 允许，并把该工具写进**当前项目**的 `.claude/settings.local.json` |
| `→` | 拒绝更改或指令（红）| 拒绝 |
| `B` | — | 退回首页，不做决定 |

决定通过 `PermissionRequest` 钩子回传给 Claude Code。**钩子最多等 60 秒**
（`TRAFFIC_LIGHT_ASK_TIMEOUT` 可改）；超时或设备不在线就什么都不输出，
Claude Code 照常弹它自己的询问。

> 钩子输出的 JSON 契约（`hookSpecificOutput.decision` 的结构、`destination` 取值、
> `timeout` 以秒计、空输出 = 不表态）是**在本地 claude-code 的 bundle 里核实过的**，
> 不是照文档猜的。
>
> ⚠️ 等待期间 Claude Code 会一直卡着——这正是「用掌机接管授权」的意思。
> 不想这样就把 `~/.claude/settings.json` 里的 `PermissionRequest` 那段删掉
> （备份在 `settings.json.bak-*`）。

## 命令接口（HTTP / 蓝牙共用）

同一条命令集，两种链路各有一套外壳：HTTP 走路径，蓝牙往 RX 特征写一行文本。
语义在 `main/link_cmd.c` 里统一实现，两边**不可能跑偏**（`test/` 有主机单元测试）。

| 命令 | HTTP | 蓝牙（写 RX 特征）| 回执 |
|------|------|------------------|------|
| 红 | `POST/GET /solo/rojo` | `rojo`（别名 `red`）| `ok rojo` |
| 黄 | `POST/GET /alerta` | `alerta`（别名 `yellow`）| `ok alerta` |
| 绿 | `POST/GET /solo/verde` | `verde`（别名 `green`）| `ok verde` |
| 灭 | `POST/GET /off` | `off` | `ok off` |
| 查询 | `GET /status` | `status` | `state: rojo` |
| 摘要 | `POST /task`（正文即摘要）| `task <文本>` | `ok task` |
| 授权 | `GET /decision?timeout=N` | `decision [N]` | `allow`/`deny`/`always`/`retry`/`none` |
| 链路 | `GET /link`、`GET /link?mode=wifi\|ble` | `link [wifi\|ble]` | `link: wifi` / `ok link, restart in 1s` |

`/task?text=...` 是 `POST /task` 的 URL 编码版。`decision` 的 N 上限 120 秒，
不写默认 60；它会转黄灯并等掌机按键。`link` 不带参数是查询当前链路；带参数
与设置菜单里「连接」那一项同义，**重启生效**——所以 WiFi 模式下切到蓝牙后
先会失联，反过来蓝牙模式下切回 WiFi 也一样，等一秒重启即可。

黄灯 30 秒无人理会会自动熄灭——但**有 decision 在等的时候不会**，
否则用户还没走到掌机灯就没了。

> ⚠️ `/decision` 会占住 httpd 整个等待期间（单 task），期间别的请求排队。
> 实际用起来没问题：这时候 Claude Code 本来也卡在同一个授权上。
> 蓝牙那条路不会：命令是丢给自己的 worker 任务跑的，不占 NimBLE 的 host task。

蓝牙：设备名 `TLMIAO`，服务 `6d69616f-0001-4000-8000-00805f9b34fb`，
RX（写）`…-0002-…`、TX（notify 回执）`…-0003-…`。UUID 在固件里是小端字节数组
（`ble_link.c` 的 `BLE_UUID128_*`），钩子那边是标准字符串，**改要两边一起改**。
设备启动时会把服务 UUID 打进日志，对不上就看日志。

## 首页按键

| 键 | 效果 |
|----|------|
| `↑` 短按 | 进设置菜单 |
| `↑` 长按 | 直接进 WiFi 设置（重扫热点）|
| `A`（黄灯时）| 进授权候选界面 |

设备同时监听 UDP 4210 用于上面说的自动发现。
