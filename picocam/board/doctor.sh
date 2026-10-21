#!/bin/sh
# PicoCam self-check. Run on the board:   sh /oem/picocam/doctor.sh
# Prints what is installed, what is running and what looks wrong, in plain words. Paste the output into a bug report.
DIR=/oem/picocam
DATA=/userdata/picocam
LOG=/tmp/picocam.log
# --- recovery commands ------------------------------------------------------------------------------------------------
#   sh doctor.sh reset-password   forgot the admin password: removes the accounts, the next web visit asks for a new one
#   sh doctor.sh factory-reset    back to default settings (accounts, models and recordings are kept)
case "$1" in
reset-password)
	[ -d $DATA ] || { echo "no PicoCam data in $DATA"; exit 1; }
	printf "This removes ALL PicoCam accounts and API keys. Settings, AI models and recordings are kept. Continue? [y/N] "
	read a; [ "$a" = y ] || [ "$a" = Y ] || { echo "cancelled"; exit 1; }
	rm -f $DATA/users.json
	sh $DIR/service.sh restart >/dev/null 2>&1
	echo "Done. Open the camera's web page: it asks for a new admin password."
	echo "If the root (ssh) password was tied to the old admin password it is changed again when you set the new one."
	exit 0 ;;
factory-reset)
	printf "Reset ALL PicoCam settings to defaults (network settings too)? Accounts, models and recordings are kept. [y/N] "
	read a; [ "$a" = y ] || [ "$a" = Y ] || { echo "cancelled"; exit 1; }
	rm -f $DATA/config.json $DATA/config.json.good $DATA/config.json.bad
	sh $DIR/service.sh restart >/dev/null 2>&1
	echo "Done. The camera is back to default settings."
	exit 0 ;;
"") ;;
*) echo "usage: sh doctor.sh [reset-password | factory-reset]"; exit 1 ;;
esac

ok() { echo "  [ok]   $*"; }
bad() { echo "  [FAIL] $*"; PROBLEMS=$((PROBLEMS + 1)); }
warn() { echo "  [warn] $*"; }
free_mb() { df -k "$1" 2>/dev/null | awk 'NR>1 {a=$4} END {printf "%d", a/1024}'; }
PROBLEMS=0

echo "PicoCam doctor"
echo
echo "Board"
echo "  model:   $(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')"
echo "  chip:    $(cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' ')"
echo "  kernel:  $(uname -r)    RAM: $(awk '/MemTotal/ {printf "%d MB", $2/1024}' /proc/meminfo) usable, media memory $(awk '/CmaTotal/ {printf "%d MB", $2/1024}' /proc/meminfo)"
echo "  boot:    $(sed -n 's/.*storagemedia=\([a-z_]*\).*/\1/p' /proc/cmdline) $(grep -q '"' /proc/mtd 2>/dev/null && echo '(raw flash / SPI NAND present)')"
echo "  uptime:  $(cut -d. -f1 /proc/uptime) s"
echo
echo "Install"
if [ -x $DIR/picocam ]; then ok "binary $DIR/picocam ($(sed -n 's/.*"version":"\([^"]*\)".*/\1/p' $DIR/manifest.json 2>/dev/null))"; else bad "$DIR/picocam is missing: run the installer again"; fi
[ -f $DIR/enabled ] && ok "enabled (starts at boot)" || bad "disabled: $DIR/enabled is missing (touch it, then reboot)"
[ -d $DIR/www ] && ok "web files present" || bad "web files missing in $DIR/www"
if [ -f /etc/init.d/S95picocam ] || grep -q "picocam/enabled" /oem/usr/bin/RkLunch.sh 2>/dev/null; then ok "start-up hook present"; else bad "no start-up hook: PicoCam will not start after a reboot (run the installer again)"; fi
F=$(free_mb /oem); [ "$F" -ge 8 ] && ok "/oem free: $F MB" || warn "/oem free: $F MB (updates from the web need about 3x the package size; use the lite package)"
F=$(free_mb /userdata); [ "$F" -ge 4 ] && ok "/userdata free: $F MB" || warn "/userdata free: $F MB"
[ -d $DIR.prev ] && ok "previous version kept for rollback" || echo "  [info] no previous version stored"
[ -f $DATA/rolled-back ] && warn "the camera rolled back to the previous version after crashes ($(cat $DATA/rolled-back))"
[ -f $DATA/recovered ] && warn "the camera restored its last working configuration after crashes"
echo
echo "Running"
if pidof picocam >/dev/null; then ok "picocam running (pid $(pidof picocam))"; else bad "picocam is not running (see the log below)"; fi
pidof rkipc >/dev/null && bad "the vendor app rkipc is running and fights PicoCam for the camera: reboot, or run: killall rkipc"
[ -e /dev/mpp_service ] && ok "video encoder device present" || bad "/dev/mpp_service missing: the vendor kernel modules did not load"
[ -e /dev/rknpu ] || [ -e /dev/dri ] && ok "NPU device present" || warn "no NPU device node (AI will be unavailable)"
ls /dev/video* >/dev/null 2>&1 && ok "camera device nodes: $(ls /dev/video* | wc -l)" || bad "no /dev/video*: the camera module is not detected. Check the flat cable (contacts towards the board, locked) and reboot"
[ -e /dev/watchdog ] && ok "watchdog present" || echo "  [info] no /dev/watchdog"
echo
echo "Network"
for i in usb0 eth0 wlan0; do
	[ -d /sys/class/net/$i ] || continue
	ip=$(ip -4 addr show $i 2>/dev/null | sed -n 's/.*inet \([0-9./]*\).*/\1/p')
	echo "  $i: ${ip:-no address}  link=$(cat /sys/class/net/$i/carrier 2>/dev/null)"
done
for p in 80 443 554; do
	netstat -ln 2>/dev/null | grep -q ":$p " && ok "port $p listening" || warn "port $p not listening (may be disabled or changed in settings)"
done
echo
echo "Storage"
if ls /sys/block/mmcblk*/device/type >/dev/null 2>&1; then
	for t in /sys/block/mmcblk*/device/type; do echo "  $(echo $t | cut -d/ -f4): $(cat $t)"; done
else echo "  no SD card or eMMC detected"; fi
grep -E " /mnt/sdcard | / " /proc/mounts | awk '{print "  mounted: "$1" on "$2" ("$3")"}'
echo
echo "Last log lines ($LOG)"
tail -n 15 $LOG 2>/dev/null | sed 's/^/  /' || echo "  none"
[ -f $DATA/crash.log ] && { echo; echo "Last crash report"; tail -n 12 $DATA/crash.log | sed 's/^/  /'; }
echo
if [ $PROBLEMS -eq 0 ]; then echo "No problems found."; else echo "$PROBLEMS problem(s) found. See docs/TROUBLESHOOTING.md for what to do."; fi
