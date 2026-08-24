#!/bin/sh
# Cross-builds the Vita .vpk. Run INSIDE the gflvn-vita image from repo root:
#   docker run --rm -v "$PWD:/src" gflvn-build-vita /src/tools/build_vita.sh
set -eu
cd /src

STAGE=$(mktemp -d /tmp/gflvn-vpk.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT INT TERM
mkdir -p "$STAGE/fonts" "$STAGE/licenses"

cp assets/manifest.json "$STAGE/"
cp assets/chapters.json "$STAGE/"
python3 tools/build_pack.py "$STAGE"
cp runtime/fonts/*.ttf "$STAGE/fonts/"
cp LICENSE THIRD_PARTY_NOTICES.md "$STAGE/licenses/"
cp runtime/OFL.txt runtime/APACHE-2.0.txt "$STAGE/licenses/"

cmake -B /build -S runtime \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" > /dev/null
make -C /build -j"$(nproc)"

vita-mksfoex -s TITLE_ID=GFLVN0001 -s APP_VER="00.11" "Girl's Frontline" /build/param.sfo
vita-pack-vpk -s /build/param.sfo -b /build/gflvn.self /build/gflvn.vpk \
    -a "$STAGE/manifest.json=manifest.json" \
    -a "$STAGE/chapters.json=chapters.json" \
    -a "$STAGE/data.gfpak=data.gfpak" \
    -a "$STAGE/pack_index.json=pack_index.json" \
    -a "$STAGE/fonts=fonts" \
    -a "$STAGE/licenses=licenses" \
    -a "/src/runtime/sce_sys=sce_sys"

python3 -c 'import sys, zipfile; z=zipfile.ZipFile(sys.argv[1]); bad=z.testzip(); assert bad is None, bad' /build/gflvn.vpk

echo "vpk: /build/gflvn.vpk"
mkdir -p /src/build
cp /build/gflvn.vpk /src/build/
sha256sum /src/build/gflvn.vpk
