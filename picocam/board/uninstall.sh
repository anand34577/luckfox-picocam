#!/bin/sh
# Remove PicoCam and return to the vendor rkipc app (settings in /userdata/picocam are kept unless --purge)
sh /oem/picocam/service.sh stop 2>/dev/null
rm -f /oem/picocam/enabled /etc/init.d/S95picocam
[ -f /oem/usr/bin/RkLunch.sh.orig ] && cp -f /oem/usr/bin/RkLunch.sh.orig /oem/usr/bin/RkLunch.sh
rm -rf /oem/picocam.prev /oem/picocam.bad /oem/picocam.new
[ "$1" = "--purge" ] && rm -rf /oem/picocam /userdata/picocam
sync
echo "PicoCam removed; reboot to start rkipc"
