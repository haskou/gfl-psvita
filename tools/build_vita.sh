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
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DVPKDATA="$STAGE" > /dev/null
make -C /build -j"$(nproc)"

echo "vpk: /build/gflvn.vpk"
