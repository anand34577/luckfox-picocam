#!/bin/sh
# Builds and runs the host self-test (native g++, no board). From the repo:
#   docker run --rm -v "$PWD":/work -w /work/picocam/firmware luckfox-ai:1.1 sh tests/run.sh
set -e
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/picocam-selftest-build
mkdir -p "$OUT"
for f in third_party/mbedtls/library/*.c; do
	o="$OUT/$(basename "$f" .c).o"
	[ "$o" -nt "$f" ] || gcc -O1 -w -Ithird_party/mbedtls/include -Ithird_party/mbedtls/library -c "$f" -o "$o"
done
g++ -std=c++17 -O1 -Wall -Wno-unused-parameter -Isrc -Isrc/core -Ithird_party -Ithird_party/mbedtls/include \
	-I/opt/rknpu2/include \
	tests/selftest.cc src/net/tls.cc src/net/ipfilter.cc src/core/util.cc src/core/config.cc src/core/log.cc src/core/audit.cc \
	src/ai/detectors.cc src/ai/textvote.cc src/ai/tracker.cc src/ai/aicommon.cc src/ai/geom.cc src/app/faces.cc "$OUT"/*.o -Wl,--unresolved-symbols=ignore-all -lpthread -o "$OUT/selftest"
"$OUT/selftest"
