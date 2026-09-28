#!/usr/bin/env python3
"""假蓝牙掌机 —— 在本机蓝牙适配器上装一个 GATT 外设，冒充小喵掌机的蓝牙模式。

用途：真机不在手边时，端到端测 ~/.claude/hooks/traffic-light.py 的蓝牙链路。
服务/特征 UUID 和命令语义都照抄固件（main/ble_link.c、main/link_cmd.c），
所以钩子那边分不出真假。

    python3 test/fake_ble.py                    # 默认：decision 回 allow
    python3 test/fake_ble.py --decision deny
    python3 test/fake_ble.py --decision none --delay 3   # 模拟没人按键
    python3 test/fake_ble.py -v                 # 把收到的命令打出来

注册走 BlueZ 的 D-Bus 接口（RegisterApplication / RegisterAdvertisement），
是纯运行时的，Ctrl-C 退出即撤销，不写任何系统配置。
"""
import argparse
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLE_NAME = "TLMIAO"
UUID_SVC = "6d69616f-0001-4000-8000-00805f9b34fb"
UUID_RX = "6d69616f-0002-4000-8000-00805f9b34fb"   # 主机写命令
UUID_TX = "6d69616f-0003-4000-8000-00805f9b34fb"   # 掌机 notify 回执

BLUEZ = "org.bluez"
PROP = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"
APP = "org.bluez.GattApplication1"
SVC = "org.bluez.GattService1"
CHR = "org.bluez.GattCharacteristic1"
ADV = "org.bluez.LEAdvertisement1"
GATT_MGR = "org.bluez.GattManager1"
ADV_MGR = "org.bluez.LEAdvertisingManager1"

APP_PATH = "/tlmiao"
ADV_PATH = "/tlmiao/adv"
SVC_PATH = APP_PATH + "/service0"
RX_PATH = SVC_PATH + "/rx"
TX_PATH = SVC_PATH + "/tx"

VERBOSE = False


def log(msg):
    if VERBOSE:
        print("[假掌机] " + msg, file=sys.stderr, flush=True)


# ── 设备语义：逐条对着 main/link_cmd.c 抄 ──────────────────────────────
# 回执格式必须一模一样，不然测出来的只是「钩子能解析我自己编的格式」。

class Device:
    def __init__(self, decision, delay):
        self.state = "off"          # off | rojo | amarillo | verde
        self.task = ""
        self.decision = decision
        self.delay = delay

    def exec(self, line):
        """返回回执字符串（含结尾换行），和设备端 lc_exec 一致。"""
        parts = line.strip().split(None, 1)
        if not parts:
            return ""
        verb = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""

        if verb in ("rojo", "red"):
            self.state = "rojo"
            return "ok rojo\n"
        if verb in ("alerta", "yellow"):
            self.state = "amarillo"
            return "ok alerta\n"
        if verb in ("verde", "green"):
            self.state = "verde"
            return "ok verde\n"
        if verb == "off":
            self.state = "off"
            return "ok off\n"
        if verb == "status":
            return "state: %s\n" % self.state
        if verb == "task":
            self.task = arg
            return "ok task\n"
        if verb == "decision":
            return self.decision + "\n"
        return "err unknown\n"


# ── D-Bus GATT 对象 ────────────────────────────────────────────────────

class Service(dbus.service.Object):
    def __init__(self, bus, path):
        dbus.service.Object.__init__(self, bus, path)
        self.path = path

    def props(self):
        return {
            "UUID": dbus.String(UUID_SVC),
            "Primary": dbus.Boolean(True),
            "Includes": dbus.Array([], signature="s"),
        }

    @dbus.service.method(PROP, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        d = self.props()
        if iface != SVC or prop not in d:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.InvalidArgs", "%s %s" % (iface, prop))
        return d[prop]

    @dbus.service.method(PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props() if iface == SVC else {}

    @dbus.service.method(PROP, in_signature="ssv")
    def Set(self, iface, prop, value):
        raise dbus.exceptions.DBusException("org.bluez.Error.NotSupported", "只读")


class Characteristic(dbus.service.Object):
    def __init__(self, bus, path, uuid, flags, svc_path, on_write=None):
        dbus.service.Object.__init__(self, bus, path)
        self.path = path
        self.uuid = uuid
        self.flags = flags
        self.svc_path = svc_path
        self.on_write = on_write
        self.value = []
        self.notifying = False

    def props(self):
        return {
            "UUID": dbus.String(self.uuid),
            "Service": dbus.ObjectPath(self.svc_path),
            "Flags": dbus.Array(self.flags, signature="s"),
            "Value": dbus.Array(self.value, signature="y"),
            "Notifying": dbus.Boolean(self.notifying),
        }

    @dbus.service.method(PROP, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        d = self.props()
        if iface != CHR or prop not in d:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.InvalidArgs", "%s %s" % (iface, prop))
        return d[prop]

    @dbus.service.method(PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props() if iface == CHR else {}

    @dbus.service.method(PROP, in_signature="ssv")
    def Set(self, iface, prop, value):
        raise dbus.exceptions.DBusException("org.bluez.Error.NotSupported", "只读")

    @dbus.service.signal(PROP, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    # 主机写命令进来
    @dbus.service.method(CHR, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        data = bytes(bytearray(value))
        if self.on_write:
            self.on_write(data)

    # 主机订阅回执
    @dbus.service.method(CHR, in_signature="a{sv}")
    def StartNotify(self, options=None):
        self.notifying = True
        log("主机订阅了 TX 通知")

    @dbus.service.method(CHR)
    def StopNotify(self):
        self.notifying = False
        log("主机取消订阅")

    @dbus.service.method(CHR, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")

    def notify(self, data):
        if not self.notifying:
            log("还没订阅，回执丢弃: %r" % data)
            return False
        self.value = list(data)
        self.PropertiesChanged(CHR, {"Value": dbus.Array(self.value, signature="y")}, [])
        return True


class Application(dbus.service.Object):
    def __init__(self, bus, path, objects):
        dbus.service.Object.__init__(self, bus, path)
        self.objects = objects

    @dbus.service.method(OM, in_signature="", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {}
        for o in self.objects:
            iface = SVC if isinstance(o, Service) else CHR
            out[dbus.ObjectPath(o.path)] = {iface: o.props()}
        return out


class Advertisement(dbus.service.Object):
    def __init__(self, bus, path):
        dbus.service.Object.__init__(self, bus, path)
        self.path = path

    def props(self):
        return {
            "Type": dbus.String("peripheral"),
            "ServiceUUIDs": dbus.Array([UUID_SVC], signature="s"),
            "LocalName": dbus.String(BLE_NAME),
        }

    @dbus.service.method(PROP, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        d = self.props()
        if prop not in d:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.InvalidArgs", prop)
        return d[prop]

    @dbus.service.method(PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props() if iface == ADV else {}

    @dbus.service.method(PROP, in_signature="ssv")
    def Set(self, iface, prop, value):
        raise dbus.exceptions.DBusException("org.bluez.Error.NotSupported", "只读")

    @dbus.service.method(ADV)
    def Release(self):
        log("广播被 BlueZ 撤销")


# ── 启动 ───────────────────────────────────────────────────────────────

def find_adapter(bus):
    om = dbus.Interface(bus.get_object(BLUEZ, "/"), OM)
    for path, ifaces in om.GetManagedObjects().items():
        if ADV_MGR in ifaces:
            return path
    raise RuntimeError("没找到支持 LE 广播的适配器")


def main():
    global VERBOSE
    ap = argparse.ArgumentParser()
    ap.add_argument("--decision", default="allow",
                    choices=["allow", "deny", "always", "retry", "none"],
                    help="收到 decision 命令时回什么（none = 模拟没人按键）")
    ap.add_argument("--delay", type=float, default=0.3,
                    help="回执前等几秒，模拟设备侧的处理耗时")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    VERBOSE = args.verbose

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    adapter = find_adapter(bus)
    log("适配器: %s" % adapter)

    dev = Device(args.decision, args.delay)
    tx = Characteristic(bus, TX_PATH, UUID_TX, ["notify"], SVC_PATH)

    def on_write(data):
        line = data.decode("utf-8", "replace")
        log("收到命令: %r" % line)
        reply = dev.exec(line)
        log("回执: %r（灯=%s 摘要=%r）" % (reply, dev.state, dev.task))
        if not reply:
            return
        # 延后一点再 notify：真机上命令是 worker 任务跑完才推回执的，
        # 这里也让它晚于写响应，顺序才和真机一样。
        GLib.timeout_add(max(1, int(args.delay * 1000)),
                         lambda: (tx.notify(reply.encode("utf-8")), False)[1])

    rx = Characteristic(bus, RX_PATH, UUID_RX, ["write", "write-without-response"],
                        SVC_PATH, on_write=on_write)
    svc = Service(bus, SVC_PATH)
    app = Application(bus, APP_PATH, [svc, rx, tx])
    adv = Advertisement(bus, ADV_PATH)

    gatt = dbus.Interface(bus.get_object(BLUEZ, adapter), GATT_MGR)
    advm = dbus.Interface(bus.get_object(BLUEZ, adapter), ADV_MGR)

    def done(name):
        return lambda *a: log("%s 注册成功" % name)

    def failed(name):
        def cb(e):
            print("!! %s 注册失败: %s" % (name, e), file=sys.stderr, flush=True)
            loop.quit()
        return cb

    loop = GLib.MainLoop()
    gatt.RegisterApplication(dbus.ObjectPath(APP_PATH), {},
                             reply_handler=done("GATT"), error_handler=failed("GATT"))
    advm.RegisterAdvertisement(dbus.ObjectPath(ADV_PATH), {},
                               reply_handler=done("广播"), error_handler=failed("广播"))

    print("假掌机就绪：广播名 \"%s\"，decision=%s，Ctrl-C 退出"
          % (BLE_NAME, args.decision), flush=True)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
