#!/bin/sh
# Cross-builds the Vita .vpk. Run INSIDE the gflvn-vita image from repo root:
#   docker run --rm -v "$PWD:/src" gflvn-build-vita /src/tools/build_vita.sh
set -e
cd /src

STAGE=/tmp/vpkdata
rm -rf "$STAGE"
mkdir -p "$STAGE/img" "$STAGE/aud" "$STAGE/fonts"

cp assets/manifest.json assets/scene.ir.json "$STAGE/"
cp assets/img/*.png "$STAGE/img/"
# only the audio referenced by this scene's manifest
grep -o '"assets/aud/[^"]*"' assets/manifest.json | tr -d '"' | while read -r f; do
    cp "$f" "$STAGE/aud/"
done
cp runtime/fonts/NotoSans-Regular.ttf "$STAGE/fonts/"

cmake -B /build -S runtime \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" > /dev/null
make -C /build -j"$(nproc)"

vita-mksfoex -s TITLE_ID=GFLVN00001 -s APP_VER="00.10" "GFL VN" /build/param.sfo
vita-pack-vpk -s /build/param.sfo -b /build/gflvn.self /build/gflvn.vpk \
    -a "$STAGE/manifest.json=manifest.json" \
    -a "$STAGE/scene.ir.json=scene.ir.json" \
    -a "$STAGE/img=img" \
    -a "$STAGE/aud=aud" \
    -a "$STAGE/fonts=fonts"

echo "vpk: /build/gflvn.vpk"
