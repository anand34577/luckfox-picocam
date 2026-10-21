#!/bin/bash
# Runs board/install.sh, doctor.sh and uninstall.sh inside a busybox container that pretends to be a Luckfox image
# (fake /oem, /userdata, launcher script, 32-bit ARM uname). No board needed. Needs Docker.
#   bash picocam/tests/simulate-install.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# a fake package built from the real scripts
PKG=$WORK/pkg
mkdir -p "$PKG/lib" "$PKG/www" "$PKG/models"
head -c 8192 /dev/urandom > "$PKG/picocam"
echo x > "$PKG/lib/librknnmrt.so"
echo '<html></html>' > "$PKG/www/index.html"
echo '{}' > "$PKG/models/demo.json"; head -c 4096 /dev/urandom > "$PKG/models/demo.rknn"
echo '{"name":"picocam","version":"test"}' > "$PKG/manifest.json"
cp "$HERE"/board/{install.sh,uninstall.sh,service.sh,doctor.sh,picocam.init} "$PKG/"
cp "$PKG/picocam.init" "$PKG/S95picocam"
echo cert > "$PKG/ca-bundle.pem"

cat > "$WORK/sim.sh" <<'SIM'
set -e
fail() { echo "FAIL: $*"; exit 1; }
mkdir -p /oem/usr/bin /oem/usr/share/iqfiles /userdata /etc/init.d /fakebin
cat > /oem/usr/bin/RkLunch.sh <<'RK'
#!/bin/sh
rcS() { echo modules; }
rcS
if [ -d "/oem/usr/share/iqfiles" ]; then
	gdb -batch -x /oem/usr/share/rkipc-ae-fix.gdb --args rkipc -a /oem/usr/share/iqfiles &
else
	rkipc &
fi
RK
cp /oem/usr/bin/RkLunch.sh /tmp/RkLunch.orig
# pretend to be a 32-bit ARM board
printf '#!/bin/sh\n[ "$1" = "-m" ] && echo armv7l || /bin/uname "$@"\n' > /fakebin/uname
chmod +x /fakebin/uname
export PATH=/fakebin:$PATH

echo "--- install"
sh /pkg/install.sh
[ -f /oem/picocam/enabled ] || fail "not enabled"
[ -x /oem/picocam/picocam ] || fail "binary missing"
[ -f /oem/picocam/doctor.sh ] || fail "doctor.sh missing"
[ -f /oem/picocam/models/demo.rknn ] || fail "models not installed"
grep -q "picocam/enabled" /oem/usr/bin/RkLunch.sh || fail "launcher not patched"
sh -n /oem/usr/bin/RkLunch.sh || fail "patched launcher has a syntax error"
[ -f /etc/init.d/S95picocam ] || fail "init script missing"

echo "--- install again (upgrade in place)"
sh /pkg/install.sh >/dev/null
[ "$(grep -c 'picocam/enabled' /oem/usr/bin/RkLunch.sh)" = 1 ] || fail "launcher patched twice"

echo "--- doctor"
sh /oem/picocam/doctor.sh | grep -q "PicoCam doctor" || fail "doctor produced nothing"

echo "--- lite package (no models)"
rm -rf /pkg-lite && cp -r /pkg /pkg-lite && rm -rf /pkg-lite/models
rm -rf /oem/picocam
sh /pkg-lite/install.sh | grep -q "installed" || fail "lite install failed"

echo "--- /etc read-only: start from the launcher hook"
rm -f /etc/init.d/S95picocam
rm -rf /etc/init.d && ln -s /proc/self/nonexistent /etc/init.d 2>/dev/null || true
cp /tmp/RkLunch.orig /oem/usr/bin/RkLunch.sh; rm -f /oem/usr/bin/RkLunch.sh.orig
sh /pkg/install.sh | grep -q "read-only" || fail "read-only /etc not detected"
grep -q "service.sh start" /oem/usr/bin/RkLunch.sh || fail "launcher hook does not start picocam"
sh -n /oem/usr/bin/RkLunch.sh || fail "hooked launcher has a syntax error"

echo "--- unknown launcher layout and normal /etc: still installs, warns"
rm -f /etc/init.d; mkdir -p /etc/init.d
printf '#!/bin/sh\nrkipc &\n' > /oem/usr/bin/RkLunch.sh; rm -f /oem/usr/bin/RkLunch.sh.orig
sh /pkg/install.sh | grep -q "could not patch" || fail "no warning for an unknown launcher"
[ -f /etc/init.d/S95picocam ] || fail "init script missing"

echo "--- recovery: forgotten admin password"
mkdir -p /userdata/picocam; echo '{}' > /userdata/picocam/users.json; echo '{}' > /userdata/picocam/config.json
echo y | sh /oem/picocam/doctor.sh reset-password >/dev/null
[ ! -f /userdata/picocam/users.json ] || fail "accounts not removed"
[ -f /userdata/picocam/config.json ] || fail "settings were removed too"
echo n | sh /oem/picocam/doctor.sh factory-reset >/dev/null || true
[ -f /userdata/picocam/config.json ] || fail "factory reset ran although answered n"

echo "--- Ubuntu image is refused with an explanation"
echo 'NAME="Ubuntu"' > /etc/os-release
sh /pkg/install.sh 2>&1 | grep -q "Buildroot" || fail "no explanation for the Ubuntu image"
rm -f /etc/os-release

echo "--- uninstall restores the vendor launcher"
sh /oem/picocam/uninstall.sh >/dev/null
[ ! -f /oem/picocam/enabled ] || fail "still enabled"
grep -q picocam /oem/usr/bin/RkLunch.sh && fail "launcher still mentions picocam" || true
echo "ALL OK"
SIM

docker run --rm -v "$PKG":/pkg:ro -v "$WORK/sim.sh":/sim.sh:ro busybox sh /sim.sh
