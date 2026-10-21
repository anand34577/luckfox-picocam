#!/bin/bash
# Install PicoCam on a Luckfox Pico board over SSH. Works on Linux, macOS, WSL and Git Bash.
#
#   tools/deploy.sh                       # board at 172.32.0.93 (USB cable), newest release from GitHub
#   tools/deploy.sh -b 192.168.1.64       # board on your network
#   tools/deploy.sh -p picocam.tar.gz     # a package you built or downloaded yourself
#   tools/deploy.sh --lite                # no demo models (boards with little flash)
#   tools/deploy.sh --models              # only add the ready-made demo AI models to an installed camera
#   tools/deploy.sh --build               # compile from source first (needs Docker, see docs/BUILDING.md)
#
# The board's default login is root / luckfox. ssh asks for the password once.
set -e
cd "$(dirname "$0")/.."
BOARD=172.32.0.93
PKG=
LITE=0
BUILD=0
MODELS=0
REPO=${PICOCAM_REPO:-anand34577/luckfox-picocam}
usage() { sed -n '2,12p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
	case "$1" in
	-b | --board) BOARD=$2; shift 2 ;;
	-p | --package) PKG=$2; shift 2 ;;
	--lite) LITE=1; shift ;;
	--models) MODELS=1; shift ;;
	--build) BUILD=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown option $1"; usage 1 ;;
	esac
done
NAME=picocam; [ $LITE = 1 ] && NAME=picocam-lite; [ $MODELS = 1 ] && NAME=picocam-models

if [ $BUILD = 1 ]; then
	echo "== building (Docker)"
	docker run --rm -v "$PWD/..":/work -w /work/picocam luckfox-ai:1.1 sh build.sh all
	V=$(tr -d '\r\n' < VERSION)
	PKG=dist/picocam-$V$([ $LITE = 1 ] && echo -lite).tar.gz
fi
if [ -z "$PKG" ]; then
	PKG=dist/$NAME.tar.gz
	BASEURL="https://github.com/$REPO/releases/latest/download"
	echo "== downloading $NAME.tar.gz (latest release of github.com/$REPO)"
	mkdir -p dist
	curl -fL --progress-bar -o "$PKG" "$BASEURL/$NAME.tar.gz" ||
		{ rm -f "$PKG"; echo "!! download failed. Check your internet connection, or build it yourself (--build), or pass a file with -p"; exit 1; }
	# integrity: compare with the checksum published next to the release
	SUMS=$(curl -fsL "$BASEURL/SHA256SUMS" || true)
	WANT=$(echo "$SUMS" | awk -v n="$NAME.tar.gz" '$2 == n {print $1}')
	if [ -n "$WANT" ]; then
		if command -v sha256sum >/dev/null 2>&1; then HAVE=$(sha256sum "$PKG" | cut -d' ' -f1)
		elif command -v shasum >/dev/null 2>&1; then HAVE=$(shasum -a 256 "$PKG" | cut -d' ' -f1)
		else HAVE=$WANT; echo "   (no sha256 tool found: checksum not verified)"; fi
		[ "$HAVE" = "$WANT" ] || { rm -f "$PKG"; echo "!! the download does not match the published checksum (corrupt download?). Run again"; exit 1; }
		echo "   checksum ok"
	fi
fi
[ -f "$PKG" ] || { echo "!! package $PKG not found"; exit 1; }
gunzip -t "$PKG" 2>/dev/null || { echo "!! $PKG is not a valid .tar.gz (interrupted download?). Delete it and run again"; exit 1; }

# Boards often share the address 172.32.0.93 and get re-flashed, so their SSH host key changes all the time: do not let a stale
# known_hosts entry block the installer.
SSH=(ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "root@$BOARD")
# Where to unpack on the board: /userdata if it has room (about twice the package), else RAM (/tmp), else stop with advice. No double
# quotes in this script: it has to survive cmd.exe quoting on Windows too (deploy.ps1 sends the same text).
NEED=$(( $(wc -c < "$PKG") / 1024 * 2 + 1024 ))
STAGE="NEED=$NEED; D=/userdata/pcinst; F=\$(df -k /userdata | awk 'NR>1{a=\$4} END{print a+0}'); if [ \$F -lt \$NEED ]; then M=\$(awk '/MemAvailable/{print \$2}' /proc/meminfo); [ -z \$M ] && M=0; if [ \$M -gt \$((NEED+16000)) ]; then D=/tmp/pcinst; else echo '!! not enough free space to unpack the package. Use the lite package (--lite) or free some space'; exit 3; fi; fi; rm -rf \$D && mkdir -p \$D && gunzip -c | tar -x -C \$D"
if [ $MODELS = 1 ]; then
	REMOTE='F=$(df -k /userdata | awk '"'"'NR>1{a=$4} END{print a+0}'"'"'); [ $F -gt 16000 ] || { echo '"'"'!! not enough free space in /userdata for the models (about 12 MB). Use an SD card, or upload just the models you need in the web UI (AI > Models)'"'"'; exit 3; }; mkdir -p /userdata/picocam/models && gunzip -c | tar -x -C /userdata/picocam/models && echo models installed'
else
	REMOTE="$STAGE && sh \$D/install.sh; rc=\$?; rm -rf \$D; exit \$rc"
fi
echo "== installing $PKG on $BOARD (password: luckfox, unless you changed it)"
# one connection: the package is streamed through ssh (no scp/sftp needed), unpacked and installed
set +e
"${SSH[@]}" "$REMOTE" < "$PKG"
rc=$?
set -e
if [ $rc -eq 255 ]; then
	cat <<MSG

!! Could not reach or log in to root@$BOARD.
   - Over USB the board is 172.32.0.93. Your PC needs an address in that network (172.32.0.100, mask 255.255.255.0).
   - On a network, use the board's IP: tools/deploy.sh -b <ip>
   - Default password is luckfox. After a PicoCam first start it is the admin password you chose.
   See docs/TROUBLESHOOTING.md
MSG
	exit 1
elif [ $rc -ne 0 ]; then
	echo "!! The installer reported an error (exit $rc), see above. Nothing else was changed."
	exit $rc
fi
echo
[ $MODELS = 1 ] && echo "Done. Open AI > Models in the web UI: the demo models are listed there." || echo "Done. Open http://$BOARD/ and set the admin password."
