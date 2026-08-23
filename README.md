# gfl-psvita

Girls' Frontline story player for PS Vita. Converts the game's plaintext story
scripts (`avgtxt` format, inside Unity TextAssets) into a portable intermediate
representation, then replays them with a portable C++/SDL2 runtime — desktop
first, cross-compiled to Vita.

Design rationale lives in a local investigation document (not distributed).

## Status

| Step | Description | State |
|---|---|---|
| 01 | avgtxt parser -> typed beats (stdlib-only) | done |
| 02 | asset resolver v0 (BIN/BGM/SE/sprites -> keys + warnings) | done |
| 02b | bundle download + UnityPy extraction (docker toolchain) | done |
| 03 | IR exporter: beats -> flat event-stream scene JSON | done |
| 04 | C++/SDL2 player (desktop + Vita from one source) | done, desktop verified headless |
| 04+ | C++/SDL2 desktop player, then Vita build | — |

## Layout

```
src/gflvn/
  avgtxt.py    # parser: one script line = one beat; unknown tags degrade, never fail
  assets.py    # resolver: beat references -> asset keys against the text mirror
  ir.py        # exporter: beats -> normalized scene JSON (investigation §4 schema)
runtime/       # SDL2 VN player (single portable main.cpp; builds desktop & Vita)
docker/
  Dockerfile         # extraction toolchain image
  Dockerfile.vita    # VitaSDK cross-compile image
tools/
  fetch_bundles.py    # stdlib-only CDN downloader (gf-data-tools resdata manifests)
  extract_assets.py   # bundle -> pngs/oggs (runs inside docker: UnityPy+magick+vgmstream)
  build_vita.sh       # cross-builds gflvn.vpk (runs inside gflvn-build-vita)
test_*.py      # plain assert suites, no framework
tests/golden/  # golden parse output for -1-1-1.txt
research/      # local clones of Dimbreath/GirlsFrontlineData + gfStory-en (not committed)
assets/        # downloaded bundles + extracted assets (not committed: game content)
docs/          # technical investigation
```

## Development

```sh
pip install -e .
python3 test_avgtxt.py            # parser: golden + unit
python3 test_assets.py            # resolver
python3 test_ir.py                # IR exporter
python3 test_assets_resolved.py   # every scene asset key resolves on disk (needs extracted assets)
```

Asset pipeline (host stays clean; all dependencies live in docker):

```sh
docker build -t gflvn-tools docker/
python3 tools/fetch_bundles.py <bundleName> ...          # bundles -> assets/bundles/
docker run --rm -v "$PWD:/work" -w /work gflvn-tools \
    python3 tools/extract_assets.py -1-1-1               # -> assets/img, assets/aud, assets/manifest.json
```

## Running

Desktop (any machine with SDL2; headless smoke test works in docker):

```sh
# from repo root, after exporting the scene:
cp assets/1-1-1.ir.json assets/scene.ir.json
cmake -B build -S runtime && cmake --build build -j
./build/gflvn assets            # click / Space / Enter to advance
GFLVN_AUTO=1 ./build/gflvn ...  # auto-advance (smoke test)
```

Controls (in-game toolbar mirrors gfStory-en: Menu / Script / Log / Auto):

| Action | Desktop | Vita |
|---|---|---|
| advance text | click / Space / Enter / X | X button / tap |
| back to chapter menu | Esc or Menu/Script button | START button |
| history log | Log button | SELECT button |
| auto-advance toggle | A key or Auto button | Y button or tap Auto |
```

PS Vita (cross-build in docker, no local toolchain):

```sh
docker build -t gflvn-build-vita -f docker/Dockerfile.vita docker/
docker run --rm -v "$PWD:/src" gflvn-build-vita /src/tools/build_vita.sh
# -> build/gflvn.vpk (title id GFLVN00001); install with VitaShell
```

Parse any script to JSON:

```sh
gflvn-parse research/GirlsFrontlineData/en-US/asset_textes/avgtxt/-1-1-1.txt
```

## Data sources

- Text scripts: [Dimbreath/GirlsFrontlineData](https://github.com/Dimbreath/GirlsFrontlineData)
  (mirrored, all locales). Parser reference:
  [shiropantsu/gfStory-en](https://github.com/shiropantsu/gfStory-en) (no license;
  used as reverse-engineering reference only).
- Art/audio bundles come from the live game CDN (`*.sunborngame.com`, verified working);
  `tools/fetch_bundles.py` archives them by name from the community resdata manifests.
