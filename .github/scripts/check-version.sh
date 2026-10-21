#!/bin/bash
# The version must be the same in picocam/VERSION, picocam/web/package.json and the newest CHANGELOG.md section.
cd "$(dirname "$0")/../.."
V=$(tr -d '\r\n' < picocam/VERSION)
P=$(sed -n 's/.*"version": "\([^"]*\)".*/\1/p' picocam/web/package.json | head -1)
C=$(sed -n 's/^## \([0-9][0-9.]*\).*/\1/p' CHANGELOG.md | head -1)
echo "VERSION=$V package.json=$P CHANGELOG=$C"
[ "$V" = "$P" ] && [ "$V" = "$C" ] || { echo "::error::version mismatch: bump all three together"; exit 1; }
