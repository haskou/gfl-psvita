# Toolchain for gfl-psvita asset extraction (step 02b).
# Everything that needs dependencies runs in here; the host stays clean.
#
# Build:  docker build -t gflvn-tools docker/
# Usage:  docker run --rm -v "$PWD:/work" -w /work gflvn-tools <cmd...>
FROM debian:trixie-slim AS vgmstream
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git cmake build-essential ca-certificates pkg-config \
        libao-dev libspeexdsp-dev libjansson-dev libvorbis-dev libopus-dev libmpg123-dev \
    && rm -rf /var/lib/apt/lists/* \
    && git clone --depth 1 https://github.com/vgmstream/vgmstream.git /src \
    && cmake -S /src -B /src/build -DBUILD_CLI=ON -DBUILD_FB2K=OFF -DBUILD_AUDACIOUS=OFF \
        -DBUILD_WASM=OFF -DUSE_AO=OFF -DUSE_FFMPEG=OFF -DUSE_SPEEX=OFF \
    && cmake --build /src/build --parallel \
    && install -m755 /src/build/cli/vgmstream-cli /usr/local/bin/

FROM python:3.12-slim
COPY --from=vgmstream /usr/local/bin/vgmstream-cli /usr/local/bin/
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ffmpeg imagemagick pngquant ca-certificates build-essential \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir UnityPy

WORKDIR /work
