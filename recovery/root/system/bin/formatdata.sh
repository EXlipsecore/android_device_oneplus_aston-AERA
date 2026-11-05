#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

CUR_SL="$(getprop ro.boot.slot_suffix)"
CUR_NUM="$(/system/bin/bootctl get-current-slot 2>/dev/null)"
ACT_NUM="$(/system/bin/bootctl get-active-boot-slot 2>/dev/null)"

if [ -f /tmp/aera_vab_ota/state ] && [ -f /tmp/aera_vab_ota/snapshot-boot ] && \
   [ -n "$CUR_SL" ] && [ -n "$CUR_NUM" ] && [ -n "$ACT_NUM" ] && \
   [ "$CUR_NUM" != "$ACT_NUM" ]; then
    SAVED_SL="$(cat /tmp/aera_vab_ota/snapshot-boot 2>/dev/null)"
    if [ "$SAVED_SL" = "$CUR_SL" ]; then
        MNT_META=0
        META_DEV=/dev/block/by-name/metadata
        [ -b "$META_DEV" ] || META_DEV=/dev/block/bootdevice/by-name/metadata
        if ! grep -q " /metadata " /proc/mounts 2>/dev/null; then
            mkdir -p /metadata
            if mount -t f2fs -o noatime,nosuid,nodev,discard "$META_DEV" /metadata 2>/dev/null || \
               mount "$META_DEV" /metadata 2>/dev/null; then
                MNT_META=1
            fi
        fi
        if grep -q " /metadata " /proc/mounts 2>/dev/null; then
            rm -rf /metadata/ota 2>/dev/null
            cp -a /tmp/aera_vab_ota /metadata/ota 2>/dev/null
            mkdir -p /metadata/gsi/ota 2>/dev/null
            chown -R 0:1000 /metadata/ota /metadata/gsi 2>/dev/null
            chmod 0750 /metadata/ota /metadata/ota/snapshots 2>/dev/null
            chmod 0771 /metadata/gsi /metadata/gsi/ota 2>/dev/null
            chcon u:object_r:metadata_file:s0 /metadata 2>/dev/null
            chcon -R u:object_r:gsi_metadata_file:s0 /metadata/gsi 2>/dev/null
            chcon -R u:object_r:ota_metadata_file:s0 /metadata/ota /metadata/gsi/ota 2>/dev/null
            restorecon -RF /metadata 2>/dev/null
            sync
            /system/bin/bootctl set-snapshot-merge-status snapshotted >/dev/null 2>&1
        fi
        [ "$MNT_META" = "1" ] && umount /metadata 2>/dev/null
    fi
fi

[ -x /system/bin/aston-vab-guard ] && /system/bin/aston-vab-guard --repair
exit 0
