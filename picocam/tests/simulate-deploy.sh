#!/bin/bash
# End to end on a fake board: tools/deploy.sh -> a fake "ssh" that runs the remote command locally -> install.sh.
# Checks the staging-space logic (normal, and /userdata too small so it unpacks in RAM) and the models-only path. Needs Docker.
#   bash picocam/tests/simulate-deploy.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PKG=$WORK/pkg
mkdir -p "$PKG/lib" "$PKG/www" "$PKG/models"
head -c 200000 /dev/urandom > "$PKG/picocam"
echo x > "$PKG/lib/librknnmrt.so"; echo '<html></html>' > "$PKG/www/index.html"
echo '{}' > "$PKG/models/demo.json"; head -c 4096 /dev/urandom > "$PKG/models/demo.rknn"
echo '{"name":"picocam","version":"test"}' > "$PKG/manifest.json"
cp "$HERE"/board/{install.sh,uninstall.sh,service.sh,doctor.sh,picocam.init} "$PKG/"
cp "$PKG/picocam.init" "$PKG/S95picocam"; echo cert > "$PKG/ca-bundle.pem"
tar -czf "$WORK/picocam.tar.gz" -C "$PKG" .
tar -czf "$WORK/picocam-models.tar.gz" -C "$PKG/models" .

cat > "$WORK/sim.sh" <<'SIM'
set -e
fail() { echo "FAIL: $*"; exit 1; }
mkdir -p /oem/usr/bin /oem/usr/share/iqfiles /userdata /etc/init.d /fakebin /tmp/repo
cat > /oem/usr/bin/RkLunch.sh <<'RK'
#!/bin/sh
if [ -d "/oem/usr/share/iqfiles" ]; then
	rkipc -a /oem/usr/share/iqfiles &
else
	rkipc &
fi
RK
printf '#!/bin/sh\n[ "$1" = "-m" ] && echo armv7l || /usr/bin/uname "$@"\n' > /fakebin/uname
# the fake ssh: drop the options, run the last argument (the remote command) here, stdin passes through
cat > /fakebin/ssh <<'SSH'
#!/bin/sh
for a; do last=$a; done
exec sh -c "$last"
SSH
# a df that reports a nearly full /userdata when FAKE_LOW=1
cat > /fakebin/df <<'DF'
#!/bin/sh
if [ "$FAKE_LOW" = 1 ] && [ "$2" = /userdata ]; then printf 'Filesystem 1K-blocks Used Available Use%% Mounted on\nx 8000 7000 1000 88%% /userdata\n'; else exec /usr/bin/df "$@"; fi
DF
chmod +x /fakebin/*
export PATH=/fakebin:$PATH
cp -r /repo/picocam/tools /tmp/repo/tools; cp /repo/picocam/VERSION /tmp/repo/VERSION
cd /tmp/repo

echo "--- deploy: normal"
bash tools/deploy.sh -p /pkgs/picocam.tar.gz | tee /tmp/out1
grep -q "Done. Open http" /tmp/out1 || fail "no success message"
[ -f /oem/picocam/enabled ] || fail "not installed"
grep -q "install from /userdata/pcinst" /tmp/out1 || fail "did not unpack in /userdata"
[ ! -d /userdata/pcinst ] || fail "staging directory left behind"

echo "--- deploy: /userdata too small, unpacks in RAM"
rm -rf /oem/picocam
FAKE_LOW=1 bash tools/deploy.sh -p /pkgs/picocam.tar.gz | tee /tmp/out2
grep -q "install from /tmp/pcinst" /tmp/out2 || fail "did not fall back to RAM"
[ -f /oem/picocam/enabled ] || fail "not installed (RAM staging)"
[ ! -d /tmp/pcinst ] || fail "RAM staging directory left behind"

echo "--- deploy --models"
bash tools/deploy.sh -p /pkgs/picocam-models.tar.gz --models | tee /tmp/out3
[ -f /userdata/picocam/models/demo.rknn ] || fail "models not installed"

echo "--- a broken package is refused before anything is sent"
echo notagz > /pkgs/bad.tar.gz
bash tools/deploy.sh -p /pkgs/bad.tar.gz 2>&1 | grep -q "not a valid" || fail "bad package accepted"
echo "ALL OK"
SIM

docker run --rm -v "$HERE/..":/repo:ro -v "$WORK":/pkgs -v "$WORK/sim.sh":/sim.sh:ro debian:stable-slim bash /sim.sh
