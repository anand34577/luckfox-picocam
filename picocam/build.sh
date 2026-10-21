#!/bin/sh
# Build PicoCam: firmware (cross-compiled) + web UI, then package into dist/picocam-<version>.tar.gz (+ -lite)
#   docker run --rm -v "$PWD":/work -w /work/picocam luckfox-ai:1.1 sh build.sh [firmware|web|package|all]
set -e
cd "$(dirname "$0")"
WHAT=${1:-all}
VERSION=${VERSION:-$(cat VERSION 2>/dev/null || echo dev)}
TC=${LUCKFOX_TOOLCHAIN:-/opt/toolchain}
BUILD=${BUILD_DIR:-/tmp/picocam-build}

firmware() {
	LUCKFOX_TOOLCHAIN=$TC cmake -S firmware -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/toolchain-uclibc.cmake" \
		-DRKMPI_DIR=${RKMPI_DIR:-/opt/rkmpi} -DRKNPU2_DIR=${RKNPU2_DIR:-/opt/rknpu2} -DPICOCAM_VERSION="$VERSION" >/dev/null
	make -C "$BUILD" -j"$(nproc)" 2>&1 | grep -E -A3 "error|warning|Error|\[100%\]" || true  # warnings stay visible
	test -f "$BUILD/picocam"
	mkdir -p dist/pkg/lib
	"$TC/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" -o dist/pkg/picocam "$BUILD/picocam"
	cp "${RKNPU2_DIR:-/opt/rknpu2}/armhf-uclibc/librknnmrt.so" dist/pkg/lib/
	ls -la dist/pkg/picocam
}

web() {
	cd web
	[ -d node_modules ] || npm ci --no-audit --no-fund
	npm run build
	cd ..
	rm -rf dist/pkg/www && mkdir -p dist/pkg && cp -r web/dist dist/pkg/www
	# precompress: the board serves .gz to browsers that accept it
	find dist/pkg/www -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' -o -name '*.json' \) -exec gzip -9 -k -f {} \;
}

package() {
	bash tools/package.sh
}

case "$WHAT" in
	firmware) firmware ;;
	web) web ;;
	package) package ;;
	all) firmware; web; package ;;
esac
