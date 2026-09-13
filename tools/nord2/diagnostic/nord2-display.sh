#!/system/bin/sh
exec > /tmp/nord2-display.log 2>&1
release=$(uname -r)
moddir=/lib/modules/$release
# Let the early services establish USB and the recovery return path first.
tries=0
while [ ! -b /dev/block/by-name/misc ] || [ "$(getprop sys.usb.state)" != adb ]; do
    [ "$tries" -lt 60 ] || exit 1
    sleep 1
    tries=$((tries + 1))
done
while read -r module; do
    [ -n "$module" ] || continue
    echo "Loading $module"
    modprobe -v -d "$moddir" "$module" || exit 1
done < /nord2/modules.display
# A deferred DSI probe unregisters its panel child. Retry now that the
# panel module and its GPIO/regulator providers have all registered.
for device in /sys/bus/platform/devices/*1400e000*; do
    [ -e "$device" ] || continue
    [ -L "$device/driver" ] && continue
    echo "${device##*/}" > /sys/bus/platform/drivers_probe || exit 1
done
tries=0
while [ ! -c /dev/dri/card0 ]; do
    [ "$tries" -lt 20 ] || exit 1
    sleep 1
    tries=$((tries + 1))
done
setprop sys.nord2.display ready
if [ -L /sys/bus/i2c/devices/0-0038/driver ]; then
    setprop sys.nord2.touch bound
fi
echo '<6>nord2-display: boot-time display modules ready' > /dev/kmsg
