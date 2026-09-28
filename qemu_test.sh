#!/usr/bin/env bash
# QEMU 无硬件验证：跑无 WiFi 版，内置时间轴会自动走一遍 UI 并打印屏幕字符画
set -u
cd "$(dirname "$0")"
QEMU=$HOME/.espressif/tools/qemu-xtensa/esp-develop-9.2.2-20250817/qemu/bin/qemu-system-xtensa
IMG=build_qemu/traffic-light-qemu-merged.bin
OUT=/tmp/tl_out
rm -f $OUT

timeout 125 $QEMU -M esp32 -m 4M -nographic \
        -drive file=$IMG,if=mtd,format=raw > $OUT 2>&1

echo "==== 异常行数（wdt/abort/assert/Guru/reboot）===="
grep -ciE "wdt|abort|assert|Guru|reboot" $OUT
echo "==== 应用日志 ===="
grep -a "traffic: " $OUT
echo "==== 屏幕快照见 $OUT ===="
