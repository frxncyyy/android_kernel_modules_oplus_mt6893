#!/system/bin/sh
exec > /tmp/nord2-diag.log 2>&1
echo '<6>nord2-diag: userspace reached' > /dev/kmsg
uname -a
release=$(uname -r)
if [ -d /lib/modules/"$release" ]; then
    while read -r module; do
        [ -n "$module" ] || continue
        echo "Loading $module"
        modprobe -v -d /lib/modules/"$release" "$module"
        echo "Result: $?"
    done < /nord2/modules.usb
fi
# The installed 4.19 recovery uses the legacy connection gate.
for cmode in /sys/class/udc/*/device/cmode; do
    [ -f "$cmode" ] && echo 2 > "$cmode"
done
tries=0
while [ "$tries" -lt 60 ]; do
    for node in /sys/class/udc/*; do
        [ -e "$node" ] || continue
        udc=${node##*/}
        echo "UDC: $udc"
        setprop sys.usb.controller "$udc"
        setprop sys.usb.config adb
        setprop ctl.start adbd
        echo '<6>nord2-diag: USB controller ready, ADB requested' > /dev/kmsg
        exit 0
    done
    sleep 1
    tries=$((tries + 1))
done
echo '<3>nord2-diag: no USB controller appeared' > /dev/kmsg
cat /sys/kernel/debug/devices_deferred
exit 1
