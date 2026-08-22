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
| 02b | Unity bundle extraction (needs archived `.ab` set) | pending data |
| 03 | IR exporter: beats -> flat event-stream scene JSON | done |
| 04+ | C++/SDL2 desktop player, then Vita build | — |

## Layout

```
src/gflvn/
  avgtxt.py    # parser: one script line = one beat; unknown tags degrade, never fail
  assets.py    # resolver: beat references -> asset keys against the text mirror
test_*.py      # plain assert suites, no framework
tests/golden/  # golden parse output for -1-1-1.txt
research/      # local clones of Dimbreath/GirlsFrontlineData + gfStory-en (not committed)
docs/          # technical investigation
```

## Development

```sh
pip install -e .
python3 test_avgtxt.py   # parser: golden + unit
python3 test_assets.py   # resolver
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
- Art/audio bundles exist only on the live game CDN and must be archived once
  (see investigation §7, "data acquisition / sunset").
