# gfl-psvita

Girls' Frontline story player for PS Vita. Converts the game's plaintext story
scripts (`avgtxt` format, inside Unity TextAssets) into a portable intermediate
representation, then replays them with a portable C++/SDL2 runtime — desktop
first, cross-compiled to Vita.

Design rationale and evidence base: [docs/investigation.md](docs/investigation.md).

## Status

| Step | Description | State |
|---|---|---|
| 01 | avgtxt parser -> typed beats (stdlib-only) | done |
| 02 | asset resolver v0 (BIN/BGM/SE/sprites -> keys + warnings) | done |
| 02b | bundle download + UnityPy extraction (docker toolchain) | done |
| 03 | IR exporter: beats -> flat event-stream scene JSON | done |
| 04+ | C++/SDL2 desktop player, then Vita build | — |

## Layout

```
src/gflvn/
  avgtxt.py    # parser: one script line = one beat; unknown tags degrade, never fail
  assets.py    # resolver: beat references -> asset keys against the text mirror
  ir.py        # exporter: beats -> normalized scene JSON (investigation §4 schema)
tools/
  fetch_bundles.py    # stdlib-only CDN downloader (gf-data-tools resdata manifests)
  extract_assets.py   # bundle -> pngs/oggs (runs inside docker: UnityPy+magick+vgmstream)
docker/Dockerfile     # extraction toolchain image
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
