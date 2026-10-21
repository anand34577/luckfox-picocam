#!/bin/sh
# PicoCam service supervisor. /etc/init.d/S95picocam is a stub that runs this file, so firmware updates can improve
# the supervisor too.
#   enable:  touch /oem/picocam/enabled      disable (back to rkipc): rm /oem/picocam/enabled; reboot
#
# Self-healing:
#   3 quick crashes in a row -> the config is put back to the last one that ran healthily for 10 minutes
#   5 quick crashes after an update -> the previous firmware version is restored
#   a crash report (exit code + the last log lines) is kept in /userdata/picocam/crash.log across reboots
DIR=/oem/picocam
DATA=/userdata/picocam
PIDFILE=/var/run/picocam-supervisor.pid
LOG=/tmp/picocam.log
export LD_LIBRARY_PATH=$DIR/lib:/oem/usr/lib:/usr/lib

protect_ssh() {
	# never let the OOM killer take away remote access
	for p in $(pidof sshd) $(pidof adbd); do echo -1000 > /proc/$p/oom_score_adj 2>/dev/null; done
}

# System > Security can switch the remote shells off; the choice is kept as marker files
apply_services() {
	[ -f $DATA/ssh.off ] && killall -q sshd dropbear 2>/dev/null
	[ -f $DATA/adb.off ] && killall -q adbd 2>/dev/null
	return 0
}

wait_media() {
	# RkLunch (S21appinit) loads the camera/encoder kernel modules in the background
	for i in $(seq 1 60); do
		[ -e /dev/mpp_service ] && [ -e /dev/rknpu ] && grep -q rkisp /proc/modules 2>/dev/null && return 0
		[ -e /dev/mpp_service ] && [ -e /dev/video11 ] && return 0
		sleep 1
	done
}

save_crash() {
	mkdir -p $DATA
	{
		echo "=== $(date) exit code $1 after $2 s"
		tail -n 40 $LOG 2>/dev/null
	} >> $DATA/crash.log
	# keep it small (flash)
	[ $(wc -c < $DATA/crash.log) -gt 131072 ] && tail -c 65536 $DATA/crash.log > $DATA/crash.log.tmp && mv $DATA/crash.log.tmp $DATA/crash.log
	return 0
}

recover() {
	if [ "$1" -eq 3 ] && [ -f $DATA/config.json.good ] && ! cmp -s $DATA/config.json $DATA/config.json.good; then
		echo "--- crash loop: restoring the last known good configuration" >> $LOG
		cp -f $DATA/config.json $DATA/config.json.bad
		cp -f $DATA/config.json.good $DATA/config.json
		echo "config" > $DATA/recovered
	elif [ "$1" -ge 5 ] && [ -d $DIR.prev ] && [ -f $DATA/update-pending ]; then
		echo "--- crash loop after an update: restoring the previous version" >> $LOG
		rm -rf $DIR.bad
		mv $DIR $DIR.bad && mv $DIR.prev $DIR
		rm -f $DATA/update-pending
		echo "update" > $DATA/rolled-back
		return 1
	fi
	return 0
}

supervise() {
	wait_media
	fast=0
	while [ -f "$DIR/enabled" ]; do
		protect_ssh
		apply_services
		killall -q rkipc 2>/dev/null
		[ -f $LOG ] && [ $(wc -c < $LOG) -gt 524288 ] && mv $LOG $LOG.1
		echo "--- start $(date)" >> $LOG
		t0=$(cut -d. -f1 /proc/uptime)
		"$DIR/picocam" -a "$DIR" -d "$DATA" >> $LOG 2>&1 &
		pid=$!
		echo $pid > /var/run/picocam.pid
		# healthy for 2 min: the update is accepted; healthy for 10 min: this configuration is "good"
		(
			sleep 120
			kill -0 $pid 2>/dev/null && rm -f $DATA/update-pending $DATA/recovered
			sleep 480
			kill -0 $pid 2>/dev/null && [ -f $DATA/config.json ] && cp -f $DATA/config.json $DATA/config.json.good
		) &
		guard=$!
		wait $pid
		rc=$?
		kill $guard 2>/dev/null
		up=$(( $(cut -d. -f1 /proc/uptime) - t0 ))
		echo "--- exit code $rc $(date)" >> $LOG
		[ $rc -ne 0 ] && save_crash $rc $up
		if [ $up -lt 30 ]; then fast=$((fast + 1)); else fast=0; fi
		if [ $fast -ge 3 ]; then
			recover $fast || { fast=0; continue; }
		fi
		if [ $fast -ge 5 ]; then echo "--- crash loop, retrying in 60 s" >> $LOG; sleep 60; fast=0; else sleep 2; fi
	done
}

case "$1" in
start)
	[ -f "$DIR/enabled" ] || { echo "picocam disabled"; exit 0; }
	# started twice (init script and the launcher hook): only one supervisor
	if [ -f $PIDFILE ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then echo "picocam already running"; exit 0; fi
	protect_ssh
	supervise </dev/null >/dev/null 2>&1 &
	echo $! > $PIDFILE
	echo "picocam started"
	;;
stop)
	[ -f $PIDFILE ] && kill $(cat $PIDFILE) 2>/dev/null
	rm -f $PIDFILE
	# SIGTERM lets the daemon disarm the hardware watchdog; SIGKILL would leave it armed (the board reboots)
	killall picocam 2>/dev/null
	for i in 1 2 3 4 5 6 7 8 9 10; do pidof picocam >/dev/null || break; sleep 1; done
	killall -9 picocam 2>/dev/null
	echo "picocam stopped"
	;;
restart)
	sh "$0" stop
	sh "$0" start
	;;
status)
	pidof picocam >/dev/null && echo "running (pid $(pidof picocam))" || echo "not running"
	tail -n 5 $LOG 2>/dev/null
	;;
*)
	echo "usage: $0 {start|stop|restart|status}"
	exit 1
	;;
esac
