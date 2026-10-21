#!/bin/bash
# Install PicoCam on a Luckfox Pico board over SSH. Works on Linux, macOS, WSL and Git Bash.
#
#   tools/deploy.sh                       # board at 172.32.0.93 (USB cable), newest release from GitHub
#   tools/deploy.sh -b 192.168.1.64       # board on your network (Pico Plus/Pro/Max/Ultra with Ethernet)
#   tools/deploy.sh -p picocam-1.2.0.tar.gz --lite
#   tools/deploy.sh --build               # compile from source first (needs Docker, see docs/BUILDING.md)
#
# The board's default login is root / luckfox. You are asked for the password by ssh (or set up a key first).
set -e
cd "$(dirname "$0")/.."
BOARD=172.32.0.93
PKG=
LITE=0
BUILD=0
REPO=${PICOCAM_REPO:-anand34577/luckfox-picocam}
usage() { sed -n '2,10p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
	case "$1" in
	-b | --board) BOARD=$2; shift 2 ;;
	-p | --package) PKG=$2; shift 2 ;;
	--lite) LITE=1; shift ;;
	--build) BUILD=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown option $1"; usage 1 ;;
	esac
done
V=$(tr -d '\r\n' < VERSION)
SUFFIX=; [ $LITE = 1 ] && SUFFIX=-lite

if [ $BUILD = 1 ]; then
	echo "== building (Docker)"
	docker run --rm -v "$PWD/..":/work -w /work/picocam luckfox-ai:1.1 sh build.sh all
fi
if [ -z "$PKG" ]; then
	PKG=dist/picocam-$V$SUFFIX.tar.gz
	if [ ! -f "$PKG" ]; then
		echo "== downloading picocam-$V$SUFFIX.tar.gz from github.com/$REPO"
		mkdir -p dist
		curl -fL --progress-bar -o "$PKG" "https://github.com/$REPO/releases/latest/download/picocam-$V$SUFFIX.tar.gz" ||
			{ rm -f "$PKG"; echo "download failed. Build it yourself (--build) or pass a package with -p"; exit 1; }
	fi
fi
[ -f "$PKG" ] || { echo "package $PKG not found"; exit 1; }

echo "== checking $BOARD"
SSH="ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new root@$BOARD"
$SSH 'echo "   connected: $(tr -d "\0" < /proc/device-tree/model)"' ||
	{ echo "cannot reach root@$BOARD. USB cable: 172.32.0.93. Network: look up the IP in your router. Default password: luckfox"; exit 1; }
echo "== installing $PKG"
# the package is streamed through ssh: no scp/sftp needed (the board's ssh server may not have it)
$SSH 'rm -rf /userdata/pcinst && mkdir -p /userdata/pcinst && gunzip -c | tar -x -C /userdata/pcinst' < "$PKG"
$SSH 'sh /userdata/pcinst/install.sh; rc=$?; rm -rf /userdata/pcinst; exit $rc'
echo
echo "PicoCam $V installed. Open http://$BOARD/ and set the admin password."
