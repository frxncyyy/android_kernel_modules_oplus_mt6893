#!/system/bin/sh
echo '<6>nord2-diag: recovery return requested after 240 seconds' > /dev/kmsg
sleep 240
if [ ! -b /dev/block/by-name/misc ]; then
    echo '<3>nord2-diag: no misc block device; keeping ADB available for rollback' > /dev/kmsg
    exit 1
fi
echo '<6>nord2-diag: requesting recovery through Android init and BCB' > /dev/kmsg
setprop sys.powerctl reboot,recovery
