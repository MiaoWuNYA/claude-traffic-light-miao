#!/usr/bin/env bash
# 一键构建两种固件变体（含合并镜像）
#   ./build.sh          → WiFi 版，真机用，输出 build/traffic-light-miao-merged.bin
#   ./build.sh qemu     → 无 WiFi 版，QEMU 验证用，输出 build_qemu/traffic-light-qemu-merged.bin
#
# 注意：两种变体共用项目根目录的 sdkconfig，构建前必须删掉让它按
# SDKCONFIG_DEFAULTS 重新生成，否则会沿用上一次变体的配置。
set -e
cd "$(dirname "$0")"

export IDF_PATH="$HOME/Documents/喵claude/repos/esp-idf"
PY="$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/python"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" > /dev/null

if [ "$1" = "qemu" ]; then
    B=build_qemu
    OUT=$B/traffic-light-qemu-merged.bin
    rm -rf "$B" sdkconfig
    idf.py -B "$B" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
else
    B=build
    OUT=$B/traffic-light-miao-merged.bin
    rm -rf "$B" sdkconfig
    idf.py -B "$B" build
fi

# 按设备实测布局合并：bootloader 0x1000、分区表 0x8000、app 在 factory 0x10000
# （设备没有 otadata/ota_0，所以没有 ota_data_initial.bin）
# --fill-flash-size 4MB：补满 4MB，QEMU 只认 2/4/8/16MB 整数的 flash 镜像
$PY -m esptool --chip esp32 merge_bin -o "$OUT" --flash_mode dio --flash_size 4MB \
    --fill-flash-size 4MB \
    0x1000   "$B/bootloader/bootloader.bin" \
    0x8000   "$B/partition_table/partition-table.bin" \
    0x10000  "$B/traffic-light-miao.bin"

ls -l "$OUT"
