#!/bin/bash
# Packages an already built firmware (dist/pkg/picocam) and web UI (web/dist) into
#   dist/picocam-<version>.tar.gz        everything, with the demo AI models (~10 MB)
#   dist/picocam-<version>-lite.tar.gz   without models, for boards with little flash (upload your own in AI > Models)
#   bash tools/package.sh           (from the picocam/ folder, e.g. inside WSL or the build container)
set -e
cd "$(dirname "$0")/.."
V=$(tr -d '\r\n' < VERSION)
[ -f dist/pkg/picocam ] || { echo "build the firmware first (build.sh firmware)"; exit 1; }
[ -d web/dist ] || { echo "build the web UI first (cd web && npm run build)"; exit 1; }
rm -rf dist/pkg/www && cp -r web/dist dist/pkg/www
# precompress: the board serves .gz to browsers that accept it
find dist/pkg/www -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -exec gzip -9 -k -f {} \;
rm -rf dist/pkg/models && mkdir -p dist/pkg/models dist/pkg/lib
cp models/* dist/pkg/models/
cp board/picocam.init dist/pkg/S95picocam
cp board/service.sh board/ca-bundle.pem board/install.sh board/uninstall.sh board/doctor.sh dist/pkg/
sed -i 's/\r$//' dist/pkg/S95picocam dist/pkg/service.sh dist/pkg/install.sh dist/pkg/uninstall.sh dist/pkg/doctor.sh
SUM=$(sha256sum dist/pkg/picocam | cut -d' ' -f1)
printf '{"name":"picocam","version":"%s","built":"%s","arch":"armv7","sha256":{"picocam":"%s"}}\n' "$V" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$SUM" > dist/pkg/manifest.json
tar -czf "dist/picocam-$V.tar.gz" -C dist/pkg .
tar -czf "dist/picocam-$V-lite.tar.gz" -C dist/pkg --exclude='./models/*' .
ls -la "dist/picocam-$V.tar.gz" "dist/picocam-$V-lite.tar.gz"
cat dist/pkg/manifest.json
# the demo models alone (for lite installs: deploy.sh --models, or upload them in AI > Models one by one)
tar -czf "dist/picocam-models.tar.gz" -C models .
ls -la dist/picocam-models.tar.gz
