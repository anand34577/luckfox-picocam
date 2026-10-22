#!/bin/sh
# Remove PicoCam and return to the vendor rkipc app (settings in /userdata/picocam are kept unless --purge)
sh /oem/picocam/service.sh stop 2>/dev/null
# the system partition back to writable, and the vendor boot as it was
mount -o remount,rw / 2>/dev/null
rm -f /etc/picocam.ro /etc/picocam-root.sh
[ -f /etc/inittab.picocam-orig ] && mv -f /etc/inittab.picocam-orig /etc/inittab
R=/lib/udev/rules.d/61-sd-cards-auto-mount.rules
[ -f $R.picocam-off ] && mv -f $R.picocam-off $R
rm -f /oem/picocam/enabled /etc/init.d/S95picocam
for f in /etc/init.d/off.S*; do [ -f "$f" ] && mv -f "$f" "/etc/init.d/${f##*/off.}"; done
[ -f /oem/usr/bin/RkLunch.sh.orig ] && cp -f /oem/usr/bin/RkLunch.sh.orig /oem/usr/bin/RkLunch.sh
rm -rf /oem/picocam.prev /oem/picocam.bad /oem/picocam.new
[ "$1" = "--purge" ] && rm -rf /oem/picocam /userdata/picocam
sync
echo "PicoCam removed; reboot to start rkipc"
