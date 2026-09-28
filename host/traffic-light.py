#!/usr/bin/env python3
"""traffic-light-miao × Claude Code 状态灯钩子

把 Claude Code 的运行状态推到小喵掌机的屏幕上：
    rojo / red     → /solo/rojo   红灯   Claude 正在干活（顺带把用户输入推成任务摘要）
    alerta/yellow  → /alerta      黄灯   需要你确认（通知/权限）
    verde / green  → /solo/verde  绿灯   任务完成
    off            → /off         全灭
    status         → /status      查询（手动用，别挂到钩子上）
    task <文本>    → /task        手动推一条任务摘要
    ask            → /decision    授权接管：挂 PermissionRequest，等掌机上的按键

两条链路，掌机上「设置 → 连接」切哪条就用哪条，这里自动跟：
    WiFi：UDP 广播找 IP（设备回 "TLMIAO <ip>"）→ HTTP :80
    蓝牙：BLE GATT 外设 "TLMIAO" → 往 RX 特征写命令，从 TX 特征收 notify 回执
命令集两边完全一样，由固件的 main/link_cmd.c 保证语义一致。
链路选择缓存在 ~/.cache/traffic-light-miao/link，探到哪条通就记哪条。

重要：钩子（尤其 UserPromptSubmit）会把 stdout 注入上下文，所以默认一声不吭，
      只在加 -v 时才打印。所有网络失败都静默吞掉，绝不阻塞 Claude。
      推状态还会 fork 到后台进程去发——蓝牙连一次要一两秒，不能卡在用户按回车
      的那条路径上。唯一的例外是 ask，它就是要阻塞等按键，但设备不可达时立刻放弃。
"""
import fcntl
import json
import os
import socket
import subprocess
import sys
import time
from urllib.request import Request, urlopen

PORT = 4210
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")),
                     "traffic-light-miao")
IP_FILE = os.path.join(CACHE, "ip")
STAMP = os.path.join(CACHE, "stamp")
LINK_FILE = os.path.join(CACHE, "link")          # "wifi" | "ble"
BLE_ADDR_FILE = os.path.join(CACHE, "ble_addr")  # 掌机蓝牙地址，缓存了就不用每次扫
PUSH_FILE = os.path.join(CACHE, "push.seq")      # 后台推送的序号，用来保序
REDISCOVER_AFTER = 15.0      # 秒；设备不在线时别每次都卡着找
HTTP_PORT = os.environ.get("TRAFFIC_LIGHT_HTTP_PORT", "80")   # 只给自测用，固件就是 80

ENDPOINTS = {
    "rojo": "/solo/rojo", "red": "/solo/rojo",
    "alerta": "/alerta", "yellow": "/alerta",
    "verde": "/solo/verde", "green": "/solo/verde",
    "off": "/off",
    "status": "/status",
    "decision": "/decision",
}

# 蓝牙链路。UUID 必须和固件 main/ble_link.c 里的完全一致——
# 那边是 BLE_UUID128_INIT 的小端字节数组，肉眼对不出来，改了要两边一起改。
BLE_NAME = "TLMIAO"
BLE_SVC = "6d69616f-0001-4000-8000-00805f9b34fb"
BLE_RX = "6d69616f-0002-4000-8000-00805f9b34fb"   # 主机写命令
BLE_TX = "6d69616f-0003-4000-8000-00805f9b34fb"   # 掌机 notify 回执
BLE_PYLIB = os.path.expanduser("~/.local/share/traffic-light-miao/pylib")
BLE_SCAN = 4.0               # 扫不到设备就算了，别一直扫
BLE_CONNECT = 8.0

PROBE_TIMEOUT = 4.0          # 探活：/decision 一挂就是几十秒，不通得赶紧撤
PUSH_TIMEOUT = 6.0
TASK_MAX = 30                # 摘要截断长度（字符）


def log(msg, verbose):
    if verbose:
        print(msg, file=sys.stderr)


# ── 小文件读写 ──────────────────────────────────────────────────────────

def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def write_text(path, txt):
    try:
        os.makedirs(CACHE, exist_ok=True)
        with open(path, "w") as f:
            f.write(txt)
    except OSError:
        pass


def link_get():
    v = read_text(LINK_FILE)
    return v if v in ("wifi", "ble") else "wifi"


def link_set(how):
    if how != link_get():
        write_text(LINK_FILE, how)


# ── WiFi 链路 ───────────────────────────────────────────────────────────

def http_get(ip, path, timeout=0.8):
    with urlopen("http://%s:%s%s" % (ip, HTTP_PORT, path), timeout=timeout) as r:
        return r.read(128).decode("utf-8", "replace")


def http_post(ip, path, data=b"", timeout=0.8):
    req = Request("http://%s:%s%s" % (ip, HTTP_PORT, path), data=data,
                  headers={"Content-Type": "text/plain; charset=utf-8"}, method="POST")
    with urlopen(req, timeout=timeout) as r:
        return r.read(128).decode("utf-8", "replace")


def discover(budget=0.8):
    """UDP 广播找设备，返回 IP 或 None。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.2)
        try:
            s.sendto(b"TLMIAO?", ("255.255.255.255", PORT))
        except OSError:
            return None
        end = time.time() + budget
        while time.time() < end:
            try:
                data, _ = s.recvfrom(64)
            except (socket.timeout, OSError):
                continue
            if data.startswith(b"TLMIAO "):
                return data.split()[1].decode()
    finally:
        s.close()
    return None


def resolve_ip(verbose):
    """缓存 IP 优先，连不上才广播发现（带限流），返回 IP 或 None。"""
    ip = read_text(IP_FILE)
    if ip:
        try:
            http_get(ip, "/status")
            return ip
        except Exception as e:
            log("缓存 IP %s 不通（%s），重新发现" % (ip, e), verbose)

    # 缓存失效才去找设备，且限流，免得设备关机后每个钩子都卡一秒
    try:
        last = os.path.getmtime(STAMP)
    except OSError:
        last = 0.0
    if time.time() - last < REDISCOVER_AFTER:
        log("跳过发现（%.0fs 前刚找过）" % (time.time() - last), verbose)
        return None
    write_text(STAMP, "")

    ip = discover()
    if not ip:
        log("广播没找到设备（掌机开机并连上热点了吗？）", verbose)
        return None
    write_text(IP_FILE, ip)
    log("发现设备: %s" % ip, verbose)
    return ip


def wifi_send(verb, arg, timeout, verbose):
    """走 HTTP 发一条命令。设备不可达返回 None。"""
    ip = resolve_ip(verbose)
    if not ip:
        return None
    if verb == "task":
        return http_post(ip, "/task", arg.encode("utf-8"), timeout)
    path = ENDPOINTS[verb]
    if verb == "decision" and arg:
        path += "?timeout=" + arg
    return http_get(ip, path, timeout)


# ── 蓝牙链路 ────────────────────────────────────────────────────────────

def ble_cmd(line, timeout):
    """连上掌机的 GATT，把 line 写进 RX，等 TX 的回执。

    每条命令都新连一次、发完就断：钩子是个短命进程，维持不了长连接，
    掌机那边断开后会自动重新广播，所以这样最省事也最稳。
    设备没扫到 / 回执超时都抛异常，由调用方决定怎么办。
    """
    if BLE_PYLIB not in sys.path:
        sys.path.insert(0, BLE_PYLIB)     # bleak 装在项目自己的目录，不动系统 Python
    import asyncio
    from bleak import BleakClient, BleakScanner

    async def run():
        dev = None
        addr = read_text(BLE_ADDR_FILE)
        if addr:
            try:
                dev = await BleakScanner.find_device_by_address(addr, timeout=1.5)
            except Exception:
                dev = None          # 地址变了/适配器抽风，退回按名字扫
        if dev is None:
            dev = await BleakScanner.find_device_by_name(BLE_NAME, timeout=BLE_SCAN)
        if dev is None:
            raise RuntimeError("没扫到 %s（掌机切到蓝牙模式了吗？）" % BLE_NAME)
        write_text(BLE_ADDR_FILE, dev.address)

        loop = asyncio.get_running_loop()
        fut = loop.create_future()

        def on_notify(_ch, data):
            if not fut.done():
                fut.set_result(bytes(data))

        async with BleakClient(dev, timeout=BLE_CONNECT) as cli:
            await cli.start_notify(BLE_TX, on_notify)   # 必须先订阅再写，否则回执会漏
            await cli.write_gatt_char(BLE_RX, line.encode("utf-8"), response=True)
            data = await asyncio.wait_for(fut, timeout)
        return data.decode("utf-8", "replace").strip()

    return asyncio.run(run())


def ble_send(verb, arg, timeout, verbose):
    """走蓝牙发一条命令。回执文本直接返回（不可达是抛异常，不是 None）。"""
    del verbose
    return ble_cmd(verb if not arg else "%s %s" % (verb, arg), timeout)


# ── 链路选择 ────────────────────────────────────────────────────────────

def cmd_send(verb, arg, timeout, verbose, only=None):
    """按缓存的偏好顺序试两条链路，成功就记成首选。

    返回 (链路, 回执)；两条都不通返回 (None, None)。
    only 指定时只走那一条——decision 用它：那边超时意味着「用户没按键」，
    不是「链路不通」，换条链路再问一遍只会白等第二个超时。
    """
    pref = link_get()
    order = [only] if only else (["ble", "wifi"] if pref == "ble" else ["wifi", "ble"])
    for how in order:
        try:
            if how == "ble":
                r = ble_send(verb, arg, timeout, verbose)
            else:
                r = wifi_send(verb, arg, timeout, verbose)
        except Exception as e:
            log("%s %s 失败: %s" % (how, verb, e), verbose)
            continue
        if r is None:
            log("%s 不可达" % how, verbose)
            continue
        if how != pref:
            link_set(how)
            log("链路缓存切到 %s" % how, verbose)
        return how, r
    return None, None


# ── 后台推送：不能卡住 Claude ───────────────────────────────────────────

def _seq_read(f):
    f.seek(0)
    p = f.read(64).split()
    try:
        return int(p[0]), int(p[1])
    except (IndexError, ValueError):
        return 0, 0


def _seq_write(f, nxt, sent):
    f.seek(0)
    f.truncate()
    f.write("%d %d" % (nxt, sent))
    f.flush()


def seq_next():
    """父进程领一个递增序号，顺序就是派发顺序。拿不到号返回 0（等于不排序）。"""
    try:
        os.makedirs(CACHE, exist_ok=True)
        with open(PUSH_FILE, "a+") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            nxt, sent = _seq_read(f)
            nxt += 1
            _seq_write(f, nxt, sent)
            return nxt
    except Exception:
        return 0


def seq_claim(f, seq):
    """子进程问一句：已经有更新的命令发出去了吗？有就作废自己。

    蓝牙下两个后台进程会同时去连掌机，谁先连上、谁的命令先到是随机的。
    不挡一下的话「任务完成(绿)」可能盖掉后到的「需要你(黄)」，把要人管的
    提示吞掉——那正是最不能丢的一条。序号在父进程按派发顺序发，所以
    「已发出的比我新」= 我这条已经过时了。
    """
    if seq <= 0:
        return True
    nxt, sent = _seq_read(f)
    if sent > seq:
        return False
    _seq_write(f, nxt, seq)
    return True


def spawn_push(seq, verb, txt):
    """把推送丢给一个脱离的子进程，父进程立刻返回。

    UserPromptSubmit 卡在用户按回车的关键路径上，而蓝牙连一次要一两秒，
    绝不能在这里同步等。start_new_session 让它脱离进程组，Claude Code
    收钩子结果时不会把它一起收走；日志也就跟着丢了，要调试就手工跑 _push。
    """
    try:
        subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "_push", str(seq), verb, txt],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
        return True
    except Exception:
        return False


def task_text(info):
    """用户这次输入的前一小段，折掉换行和连续空白。"""
    txt = " ".join(((info or {}).get("prompt") or "").split())
    return txt[:TASK_MAX]


def push_now(seq, verb, txt, verbose):
    """子进程里跑：拿锁 → 确认自己没过时 → 发状态 → 顺带推摘要。"""
    os.makedirs(CACHE, exist_ok=True)
    try:
        lock = open(PUSH_FILE, "a+")
    except OSError:
        lock = None
    try:
        if lock:
            fcntl.flock(lock, fcntl.LOCK_EX)     # 顺带把两次连接串起来，蓝牙禁不起并发
            if not seq_claim(lock, seq):
                log("seq %d 已被更新的命令顶掉，跳过 %s" % (seq, verb), verbose)
                return
        how, r = cmd_send(verb, "", PUSH_TIMEOUT, verbose)
        if how is None:
            log("%s 推送失败：两条链路都不通" % verb, verbose)
            return
        log("%s → %s %r" % (verb, how, r.strip()), verbose)
        if txt and verb in ("rojo", "red"):
            cmd_send("task", txt, PUSH_TIMEOUT, verbose)
    finally:
        if lock:
            fcntl.flock(lock, fcntl.LOCK_UN)
            lock.close()


# ── 授权接管：PermissionRequest 钩子 ────────────────────────────────────
# 掌机黄灯亮起、用户按方向键 → 这里把结果翻译成 Claude Code 的授权决定。
# 设备不可达 / 用户没按键 / 任何异常 → 什么都不输出，等于「不表态」，
# Claude Code 会照常弹它自己的询问。绝不因为钩子出错而卡住 Claude。

ASK_TIMEOUT = int(os.environ.get("TRAFFIC_LIGHT_ASK_TIMEOUT", "60"))


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def emit_allow(tool, always=False):
    decision = {"behavior": "allow"}
    if always and tool:
        # 只写当前项目的 .claude/settings.local.json。
        # 不带 ruleContent = 整个工具放行，这是「总是接受更改」的字面含义。
        # （Claude Code 自己的「总是允许」对 Bash 会推前缀规则如 `git commit *`，
        #   但那依赖它内部一堆辅助函数；在这里半吊子重写有把规则放宽的风险，
        #   所以用这个可预测的形式。想收窄就自己改 .claude/settings.local.json。）
        # 这个结构是从 claude-code 的 bundle 里核实过的，不是猜的：
        #   {type:"addRules", rules:[{toolName, ruleContent?}], behavior, destination}
        #   ruleContent 可选；destination 取 localSettings 合法。
        decision["updatedPermissions"] = [{
            "type": "addRules",
            "rules": [{"toolName": tool}],
            "behavior": "allow",
            "destination": "localSettings",
        }]
    emit({"hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                 "decision": decision}})


def emit_deny(message):
    emit({"hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                 "decision": {"behavior": "deny", "message": message}}})


def read_stdin_json():
    """读钩子从 stdin 喂进来的 JSON。手工在终端跑时 stdin 是 tty，直接返回空。"""
    try:
        if sys.stdin.isatty():
            return {}
        raw = sys.stdin.read()
    except Exception:
        return {}
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except Exception:
        return {}


def cmd_ask(verbose):
    info = read_stdin_json()
    tool = info.get("tool_name") or ""
    os.makedirs(CACHE, exist_ok=True)

    # 先探活再挂长轮询：/decision 一挂就是几十秒，设备不在线得立刻放弃，
    # 不然用户白等一场。用 status 探——它不改灯，探完不留痕迹。
    how, _ = cmd_send("status", "", PROBE_TIMEOUT, verbose)
    if how is None:
        log("设备不可达，交回 Claude Code 自己问", verbose)
        return 0

    # decision 会在设备侧把灯打黄（需要你操作 → 滴两声），然后等按键。
    _, ans = cmd_send("decision", str(ASK_TIMEOUT), ASK_TIMEOUT + 10, verbose, only=how)
    ans = (ans or "").strip()
    log("掌机决定: %r（工具 %s）" % (ans, tool), verbose)

    if ans == "allow":
        emit_allow(tool)
    elif ans == "always":
        emit_allow(tool, always=True)
    elif ans == "deny":
        emit_deny("用户在小喵掌机上拒绝了这次操作。")
    elif ans == "retry":
        emit_deny("用户在小喵掌机上按了「重来」：请换个做法重新尝试，"
                  "不要原样重复刚才的操作。")
    # 其它（超时没按 / 回执丢了）→ 不表态
    return 0


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    if not argv:
        log("用法: traffic-light.py [-v] rojo|alerta|verde|off|status|task <文本>|ask", True)
        return 0
    cmd = argv[0].lower()

    # 后台推送的实际执行体，由 spawn_push 拉起来，不给人手工用
    if cmd == "_push":
        seq = int(argv[1]) if len(argv) > 1 and argv[1].isdigit() else 0
        verb = argv[2] if len(argv) > 2 else ""
        txt = argv[3] if len(argv) > 3 else ""
        if verb not in ENDPOINTS:
            log("_push 未知状态: %r" % verb, verbose)
            return 0
        push_now(seq, verb, txt, verbose)
        return 0

    if cmd == "ask":
        return cmd_ask(verbose)

    if cmd == "task":
        txt = " ".join(" ".join(argv[1:]).split())[:TASK_MAX]
        how, r = cmd_send("task", txt, PUSH_TIMEOUT, verbose)
        log("task → %s %r" % (how, (r or "").strip()), verbose)
        return 0

    if cmd not in ENDPOINTS:
        log("未知状态: %s" % argv[0], verbose)
        return 0

    os.makedirs(CACHE, exist_ok=True)

    if cmd == "status":
        _, r = cmd_send("status", "", PUSH_TIMEOUT, verbose)
        if r:
            print(r.strip())
        return 0

    # 推状态：领个号就丢给后台，父进程立刻返回
    spawn_push(seq_next(), cmd, task_text(read_stdin_json()))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)          # 钩子永远不要因为自己出错打断 Claude
