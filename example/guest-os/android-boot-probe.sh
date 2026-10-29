#!/system/bin/sh
exec > /dev/ttyS0 2>&1
export PATH=/system/bin:/system/xbin
printf '\nIIV_ANDROID_PROBE_STARTED\n'
cat /proc/sys/kernel/random/boot_id
uname -a
getprop ro.build.version.release
getprop ro.build.version.sdk
cat /proc/1/comm
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    echo "IIV_ANDROID_WAIT zygote=$(getprop init.svc.zygote) surfaceflinger=$(getprop init.svc.surfaceflinger)"
    sleep 30
done
echo IIV_ANDROID_BOOT_BEGIN
getprop ro.build.fingerprint
echo "os_id=android"
echo "os_version=$(getprop ro.build.version.release)"
echo "api_level=$(getprop ro.build.version.sdk)"
echo "boot_id=$(cat /proc/sys/kernel/random/boot_id)"
echo "pid1=$(cat /proc/1/comm)"
echo "kernel=$(uname -r)"
echo "sys.boot_completed=$(getprop sys.boot_completed)"
echo "init.svc.zygote=$(getprop init.svc.zygote)"
echo "init.svc.surfaceflinger=$(getprop init.svc.surfaceflinger)"
ps -A | grep -E 'zygote|system_server|surfaceflinger'
cat /proc/mounts | grep /data
echo IIV_ANDROID_BOOT_END
