# gfl-psvita

Unofficial Girls' Frontline story player for PlayStation Vita. The project
converts the game's `avgtxt` story scripts into a small JSON event stream and
replays it with a native-resolution (960×544) SDL2 runtime.

The Vita application includes hierarchical story selection, persistent
read/progress state, backlog and auto modes, story choices, call presentation,
scene effects, event artwork, LiveArea metadata, and direct access to a
seekable single-file asset pack.

> [!IMPORTANT]
> This repository does not contain the extracted story corpus, game artwork,
> or game audio. Those files live under the ignored `assets/` directory and
> must be obtained by the user. Generated VPKs contain that user-supplied data
> and must not be redistributed without permission from the relevant rights
> holders.

This is a fan project. It is not affiliated with or endorsed by MICA Team,
Sunborn, or Darkwinter.

## Project status

The project is alpha software. The complete current English story corpus can
be imported and packaged, and clean VPK installations boot in Vita3K. Real
hardware validation is ongoing. Story presentation is still being compared
against gfStory-en scene by scene; bug reports should include the category,
chapter, story title, and exact line where a discrepancy occurs.

## Repository layout

```text
src/gflvn/           Python avgtxt parser, asset resolver, and IR exporter
runtime/             Shared C++/SDL2 desktop and Vita player
runtime/sce_sys/     Vita icon and LiveArea resources
tools/               Import, extraction, chapter, packaging, and build tools
docker/              Reproducible extraction and VitaSDK environments
tests/               Test support and golden data
test_*.py            Unit and local-corpus acceptance tests
assets/              Generated scenes/media; ignored by Git
research/            Local upstream data/reference clones; ignored by Git
```

## Requirements

- Python 3.10 or newer
- Docker, for Unity asset extraction and Vita cross-compilation
- A legal source of the Girls' Frontline client data
- For a full English import, local clones at:

```sh
git clone https://github.com/gf-data-tools/gf-data-us.git research/gf-data-us
git clone https://github.com/shiropantsu/gfStory-en.git research/gfStory-en
```

The importer also supports the older
[`Dimbreath/GirlsFrontlineData`](https://github.com/Dimbreath/GirlsFrontlineData)
layout as a fallback.

Install the local Python package with:

```sh
python3 -m pip install -e .
```

## Importing the story corpus

Build the extraction environment once:

```sh
docker build -t gflvn-tools docker/
```

Import all supported scripts and download their referenced bundles:

```sh
python3 tools/import_chapter.py --all
docker run --rm -v "$PWD:/work" -w /work gflvn-tools \
  python3 tools/extract_assets.py
python3 tools/build_chapter_tree.py
```

The pipeline writes generated content to `assets/`. Re-running it is
incremental: existing images/audio are reused, and obsolete scene JSON files
are removed during a complete import.

Useful partial commands:

```sh
python3 tools/import_chapter.py '-1-*'       # one filename family
python3 tools/import_chapter.py --manifest   # rebuild current manifest scenes
python3 tools/fetch_bundles.py --region us --find avgpicprefabs
```

Event logos and posters can optionally be prepared from a local directory of
`<event>/logo.*` and `<event>/poster.*` pairs:

```sh
python3 -m pip install Pillow
python3 tools/import_event_art.py /path/to/event-art
python3 tools/build_chapter_tree.py
```

## Building

### PlayStation Vita

Build the VitaSDK image, then generate the VPK:

```sh
docker build -t gflvn-build-vita -f docker/Dockerfile.vita docker/
docker run --rm -v "$PWD:/src" gflvn-build-vita /src/tools/build_vita.sh
```

The result is `build/gflvn.vpk` with title ID `GFLVN0001`. The VPK contains
only a handful of installation entries: scenes, PNGs, and OGGs are stored in
one seekable `data.gfpak`, avoiding thousands of slow small-file writes in
VitaShell. The pack is read directly at runtime and is not unpacked again on
first launch.

### Desktop

Install the SDL2, SDL2_image, SDL2_mixer, and SDL2_ttf development packages,
then run:

```sh
cmake -S runtime -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop --parallel
./build/desktop/gflvn assets
```

Set `GFLVN_AUTO=1` for an automated smoke run.

## Controls

| Action | Desktop | PS Vita |
|---|---|---|
| Advance / confirm | click, Space, Enter, or X | Cross or tap |
| Hide / show story UI | — | Circle |
| Back to menu | Escape | Start during a story; Circle in menus |
| Open history | L | Square or Select |
| Toggle auto mode | A | Triangle |
| Mark chapter read/unread | Z | Square |
| Navigate | arrow keys | D-pad |
| Fast menu navigation | hold arrow key | hold D-pad |

The on-screen footer shows context-specific controls in chapter selection.

## Tests

Run every test that is available in the current checkout:

```sh
python3 tools/run_tests.py
```

Pure parser/runtime metadata tests run in a fresh clone. Corpus acceptance
tests report `SKIP` until their ignored `research/` or `assets/` prerequisites
exist. With a complete local import they additionally verify all 3,582 scenes,
chapter reachability, asset resolution, Vita-sized sprites, and pack inputs.

The GitHub Actions workflow runs the clean-checkout Python suite and compiles
the desktop runtime on every push and pull request.

## Data and implementation references

- [`gf-data-tools/gf-data-us`](https://github.com/gf-data-tools/gf-data-us):
  current English text and resource metadata.
- [`Dimbreath/GirlsFrontlineData`](https://github.com/Dimbreath/GirlsFrontlineData):
  legacy text mirror supported by the importer.
- [`shiropantsu/gfStory-en`](https://github.com/shiropantsu/gfStory-en):
  presentation and chapter-structure reference. It is consumed only from a
  user-provided local clone and is not vendored here.

## Licensing

Original source code is available under the [MIT License](LICENSE).
Third-party components, fonts, game materials, names, and trademarks retain
their own terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
