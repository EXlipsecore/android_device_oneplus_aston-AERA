#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

[ -x /system/bin/aston-vab-guard ] && /system/bin/aston-vab-guard --repair

i=0
while [ "$i" -lt 30 ]; do
    [ -e /sys/kernel/boot_adsp/boot ] && break
    sleep 0.1
    i=$((i + 1))
done
[ -e /sys/kernel/boot_adsp/boot ] && echo 1 > /sys/kernel/boot_adsp/boot 2>/dev/null

chmod 0666 /dev/kgsl-3d0 /dev/dma_heap/* 2>/dev/null
chmod 0755 /vendor/bin/hw/vendor.qti.hardware.vibrator.service.oplus 2>/dev/null
start vibrator
start aston-cpu-sensor

[ -e /sys/class/power_supply/usb/otg_switch ] && echo 1 > /sys/class/power_supply/usb/otg_switch 2>/dev/null
[ -e /proc/touchpanel/game_switch_enable ] && echo 1 > /proc/touchpanel/game_switch_enable 2>/dev/null
exit 0
