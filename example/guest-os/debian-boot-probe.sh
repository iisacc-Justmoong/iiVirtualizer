#!/bin/sh
# Wait after the Type=simple service starts so multi-user.target can settle.
sleep 15
IIV_CONSOLE=/dev/ttyAMA0
[ -c "$IIV_CONSOLE" ] || IIV_CONSOLE=/dev/ttyS0
exec > "$IIV_CONSOLE" 2>&1
printf '\nIIV_DEBIAN_BOOT_BEGIN\n'
. /etc/os-release
printf 'os_id=%s\nos_version=%s\n' "$ID" "$VERSION_ID"
printf 'pid1='; cat /proc/1/comm
printf 'multi_user='; systemctl is-active multi-user.target
printf 'kernel='; uname -r
printf 'architecture='; uname -m
printf 'boot_id='; cat /proc/sys/kernel/random/boot_id
printf 'root='; findmnt -n -o SOURCE,FSTYPE /
echo IIV_DEBIAN_BOOT_END
