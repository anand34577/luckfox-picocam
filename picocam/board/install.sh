#!/bin/sh
# PicoCam installer. Run on the board from the unpacked package directory (tools/deploy.sh and deploy.ps1 do this for you):
#   mkdir -p /userdata/pcinst && gunzip -c picocam-*.tar.gz | tar -x -C /userdata/pcinst && sh /userdata/pcinst/install.sh
# Options:
#   --no-models   do not install the bundled demo models (saves ~12 MB of flash; upload your own in AI > Models)
#   --help
# Works on Luckfox Pico, Mini, Plus, Pro, Max, Ultra and Zero (RV1103 / RV1106) with SPI NAND, eMMC or SD boot.
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
DIR=/oem/picocam
DATA=/userdata/picocam
LAUNCH=/oem/usr/bin/RkLunch.sh
WITH_MODELS=1
for a in "$@"; do
	case "$a" in
	--no-models) WITH_MODELS=0 ;;
	-h | --help) sed -n '2,8p' "$0"; exit 0 ;;
	*) echo "unknown option $a"; exit 1 ;;
	esac
done

ls "$SRC"/models/* >/dev/null 2>&1 || WITH_MODELS=0   # the lite package has none

say() { echo "== $*"; }
die() { echo "!! $*" >&2; exit 1; }
free_kb() { df -k "$1" 2>/dev/null | awk 'NR>1 && NF>=3 {a=$(NF-2)} END {print a+0}'; }
size_kb() { du -sk "$@" 2>/dev/null | awk '{s+=$1} END {print s+0}'; }

say "PicoCam install from $SRC"

# ---- 1. is this a supported board?
[ -f "$SRC/picocam" ] && [ -f "$SRC/service.sh" ] || die "package is incomplete (picocam or service.sh missing)"
case "$(uname -m)" in
armv7* | arm) ;;
*) die "this package is for the 32-bit ARM Luckfox boards (uname says $(uname -m)). Run it on the board, not on your PC" ;;
esac
if grep -qi ubuntu /etc/os-release 2>/dev/null; then
	die "this is the Ubuntu image. PicoCam needs the Luckfox Buildroot image (the one with the camera demo, rkipc). Flash that image first: docs/INSTALL.md, step 1"
fi
[ -d /oem ] || die "/oem not found: this does not look like a Luckfox Buildroot image. Flash the Buildroot image first: docs/INSTALL.md, step 1"
[ -d /userdata ] || die "/userdata not found: this does not look like a Luckfox buildroot image"
touch /oem/.pc_rw 2>/dev/null && rm -f /oem/.pc_rw || die "/oem is read-only. Remount it read-write (mount -o remount,rw /oem) and retry"
MODEL=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')
RAM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
BOOT=$(sed -n 's/.*storagemedia=\([a-z_]*\).*/\1/p' /proc/cmdline)
grep -q '"' /proc/mtd 2>/dev/null && [ -z "$BOOT" ] && BOOT=spi-nand
echo "   board: ${MODEL:-unknown}   usable RAM: $((RAM_KB / 1024)) MB   boot medium: ${BOOT:-unknown}"
[ "$RAM_KB" -lt 40000 ] && echo "   warning: very little RAM; the AI pipeline may not fit (live view and recording still work)"

# ---- 2. is there room? (flash partitions on SPI NAND boards are small)
/etc/init.d/S95picocam stop 2>/dev/null || true
CORE_KB=$(size_kb "$SRC/picocam" "$SRC/lib" "$SRC/www" "$SRC/service.sh" "$SRC/install.sh" "$SRC/uninstall.sh" "$SRC/doctor.sh" "$SRC/ca-bundle.pem")
MODELS_KB=$(size_kb "$SRC/models")
OLD_KB=$(size_kb $DIR)
FREE_KB=$(free_kb /oem)
AVAIL_KB=$((FREE_KB + OLD_KB))
MODELS_AT=$DIR/models
if [ "$WITH_MODELS" = 1 ]; then
	# firmware updates keep the old version for rollback, so ask for room for two copies of the core plus the models
	if [ $AVAIL_KB -lt $((CORE_KB * 2 + MODELS_KB + 1024)) ]; then
		U_FREE=$(free_kb /userdata)
		if [ "$U_FREE" -gt $((MODELS_KB + 2048)) ] && [ "$BOOT" != "spi-nand" ]; then
			MODELS_AT=$DATA/models
			echo "   /oem is small ($((AVAIL_KB / 1024)) MB free): bundled models go to $MODELS_AT"
		else
			echo "   not enough flash for the bundled models ($((MODELS_KB / 1024)) MB): installing without them."
			echo "   Upload your own .rknn models in the web UI (AI > Models)."
			WITH_MODELS=0
		fi
	fi
fi
[ $AVAIL_KB -lt $((CORE_KB + 512)) ] && die "not enough space in /oem: $((AVAIL_KB / 1024)) MB free, $((CORE_KB / 1024)) MB needed"

# ---- 3. files
mkdir -p $DIR/lib $DATA
cp -f "$SRC/picocam" $DIR/picocam.new && mv -f $DIR/picocam.new $DIR/picocam
chmod +x $DIR/picocam
cp -f "$SRC/lib/"* $DIR/lib/
rm -rf $DIR/www && cp -r "$SRC/www" $DIR/www
if [ "$WITH_MODELS" = 1 ]; then
	mkdir -p "$MODELS_AT" && cp -f "$SRC/models/"* "$MODELS_AT/"
else
	mkdir -p $DIR/models
fi
cp -f "$SRC/manifest.json" "$SRC/install.sh" "$SRC/uninstall.sh" "$SRC/service.sh" "$SRC/doctor.sh" $DIR/
[ -f "$SRC/ca-bundle.pem" ] && cp -f "$SRC/ca-bundle.pem" $DIR/
tr -d '\r' < "$DIR/service.sh" > "$DIR/service.sh.tmp" && mv -f "$DIR/service.sh.tmp" "$DIR/service.sh"

# ---- 4. start at boot. Normal images: /etc/init.d/S95picocam. If /etc is read-only the hook in RkLunch.sh (below) starts it.
if touch /etc/init.d/.pc_rw 2>/dev/null; then
	rm -f /etc/init.d/.pc_rw
	tr -d '\r' < "$SRC/S95picocam" > /etc/init.d/S95picocam
	chmod +x /etc/init.d/S95picocam
	INIT_OK=1
else
	INIT_OK=0
	echo "   /etc/init.d is read-only: PicoCam will be started from $LAUNCH instead"
fi

# ---- 5. stop the vendor camera app (rkipc) from starting; the vendor launcher still loads the kernel modules
HOOK_OK=0
if [ -f $LAUNCH ]; then
	[ -f $LAUNCH.orig ] || cp $LAUNCH $LAUNCH.orig
	if ! grep -q "picocam/enabled" $LAUNCH; then
		awk -v start="$INIT_OK" '/if \[ -d "\/oem\/usr\/share\/iqfiles" \]; then/ && !done {
			print "\tif [ -f /oem/picocam/enabled ]; then"
			print "		: # PicoCam runs the camera"
			if (start != 1) print "\t\tsh /oem/picocam/service.sh start"
			sub(/if/, "elif"); print; done=1; next }
			{ print }' $LAUNCH.orig > $LAUNCH.new
		sed -i 's#gdb -batch -x /oem/usr/share/rkipc-ae-fix.gdb --args ##' $LAUNCH.new
		if grep -q "picocam/enabled" $LAUNCH.new && sh -n $LAUNCH.new; then
			chmod +x $LAUNCH.new && mv -f $LAUNCH.new $LAUNCH
		else
			rm -f $LAUNCH.new
		fi
	fi
	grep -q "picocam/enabled" $LAUNCH && HOOK_OK=1
else
	echo "   $LAUNCH not found (other image layout)"
fi
if [ $HOOK_OK = 0 ]; then
	echo "   note: could not patch the vendor launcher. PicoCam stops rkipc itself at start-up, which works but may add a few"
	echo "   seconds at boot. If the camera does not come up after a reboot, see docs/TROUBLESHOOTING.md"
	[ $INIT_OK = 0 ] && die "no way to start PicoCam at boot on this image (read-only /etc and unknown launcher). Please open an issue with the output of: cat $LAUNCH; ls /etc/init.d /oem/usr/etc/init.d"
fi

# ---- 6. vendor ISP tuning file: PicoCam tunes exposure at runtime, keep the factory IQ file
for f in /oem/usr/share/iqfiles/*.json.orig; do
	[ -f "$f" ] && cp -f "$f" "${f%.orig}"
done
rm -f /oem/usr/share/rkipc-ae-fix.gdb

touch $DIR/enabled
sync
say "installed $(cat $DIR/manifest.json)"
killall -q rkipc gdb 2>/dev/null || true
setsid sh $DIR/service.sh start </dev/null >/dev/null 2>&1
sleep 2
echo "== open the web page and set the admin password (first start takes up to a minute):"
for i in eth0 usb0 wlan0; do
	ip=$(ifconfig $i 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
	[ -n "$ip" ] && echo "     http://$ip/    ($i)"
done
echo "== something odd?  sh $DIR/doctor.sh      back to the stock app:  sh $DIR/uninstall.sh && reboot"
