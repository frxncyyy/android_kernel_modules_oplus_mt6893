#!/system/bin/sh
exec > /tmp/nord2-storage.log 2>&1
release=$(uname -r)
while read -r module; do
    [ -n "$module" ] || continue
    echo "Loading $module"
    modprobe -v -d /lib/modules/"$release" "$module"
    echo "Result: $?"
done < /nord2/modules.storage
ls -l /dev/block/by-name/misc /dev/block/by-name/boot /dev/block/by-name/recovery
