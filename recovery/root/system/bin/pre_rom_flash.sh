#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

rm -rf /tmp/aera_vab_ota 2>/dev/null
if [ -f "$1" ] && head -c 2097152 "$1" 2>/dev/null | grep -aq "my_product"; then
    [ -x /system/bin/aston-vab-guard ] && /system/bin/aston-vab-guard --precreate-oplus
fi
exit 0
