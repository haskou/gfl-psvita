# GFL Story Edition for PS Vita — Technical Investigation

Evidence base: local clones of [Dimbreath/GirlsFrontlineData](https://github.com/Dimbreath/GirlsFrontlineData) (476 MB, all locales) and [shiropantsu/gfStory-en](https://github.com/shiropantsu/gfStory-en) (full unpack + replay pipeline), in `research/`. All claims below point at concrete files; tag frequencies were measured by scanning the actual data (script in appendix A).

---

## 1. Repository / data analysis

### 1.1 Where story content lives

The **current** GFL story format is *not* a binary or compiled format. It is a line-based plaintext script (`avgtxt`) stored as Unity `TextAsset`s inside the bundle `asset_textavg.ab`, container path `assets/resources/dabao/avgtxt/<name>.txt`.

- `GirlsFrontlineData` mirrors these already extracted as **plain text**, per locale:
  - `GirlsFrontlineData/en-US/asset_textes/avgtxt/**/*.txt` — 1,945 files (main story, fetter/doll stories, skin stories, theater, VA-11 collab, startavg, anniversary…)
  - Identical sets exist under `zh-CN/`, `ja-JP/`, `ko-KR/`.
- File naming encodes structure: main story `-<chapter>-<map>-<node>.txt` (e.g. `-1-1-1.txt`, `-1-2-2first.txt` / `-1-2-2end.txt` for pre/post-battle halves); fetter `fetter/<dollId>.txt`; anniversary `<gunId>.txt`; skin `<skinId>.txt`.

**Localization**: the four locales contain the *same 1,232 top-level filenames* (verified programmatically). Same script skeleton, translated dialogue text. Language selection = ship multiple locale text packs; they're tiny (KB-scale per script). A separate keyed table `asset_textes/avglanguage/language_avg_<loc>.txt` (`id|rich-text`) holds titles/credits strings.

### 1.2 Script format

One line = one beat:

```
<narrators>||<effects>:<content>
```

Real example (`en-US/asset_textes/avgtxt/-1-1-1.txt`):

```
UMP45(0)<Speaker>UMP45</Speaker><通讯框>||<黑屏2><BIN>3</BIN><Night><SE1>Battlefield</SE1>:Calling UMP9, is 416 dead yet?
HK416(2)<Speaker>416</Speaker>;UMP45(0)||:Sorry to disappoint. Tell me you're going to withdraw us now.
 ()||<BGM>BGM_Room</BGM><BIN>27</BIN><边框>2</边框>:Five days after AR-15 has gone missing.
```

Grammar (reverse-engineered, confirmed against `gfStory-en/unpack/src/gfunpack/stories.py` `StoryTranspiler`):

- **narrators**: `Name(exprIndex)` separated by `;` — every character visible on stage this line, first-listed conventionally speaks; optional `<Speaker>Display Name</Speaker>` overrides the shown name (e.g. `HK416(2)` displays as "416"). Empty narrator = narration line. 12,618 of 52,072 lines carry multiple sprites.
- **effects** (between `||` and `:`): inline tags `<TAG>…</TAG>`, see §2.
- **content**: dialogue pages split by `+`; Unity rich text (`<color=#RRGGBBAA>`, `<size=n>`); option markers embedded in content: `<c>` (simple choice), `<r>`/`<t>` (repeating/looping variants), `<cg>` (click-on-CG choice), `<va11>` (collab minigame).
- **`<分支>N</分支>`** (branch): line only plays if choice variable == N. This is the entire branching mechanism.

### 1.3 Chapter / event metadata

Structured tables (in Dimbreath under `stc/` and `asset_textes/table/`; gfStory-en reads them as hjson/json from its downloaded dump `gf-data-us/formatted/`):

| Content type | Table | Notes |
|---|---|---|
| Main chapters | `story_playback` | id, campaign id, chapter no., title, order |
| Event story nodes | `StcStory_util` / `story_util` | per-event `scripts` list, `start`/`first`/`end` script ids, descriptions |
| Missions | `mission` | stage names |
| Doll stories | `fetter` / `fetter_story` | bonding chapters & unlock milestones |
| MOD/Neural upgrade | `mindupdate_story_info` | gun id, stage id, script list |
| Skins/others | `skin`, `gun`, `npc`, `sangvis` | name/id resolution |

`chapters.py` in gfStory-en shows exactly how these join together to map every script file into a browsable tree — including the messy parts (see §7).

### 1.4 What does *not* change: old vs new formats

- The avgtxt line format is **the same across the game's whole lifetime** — 2016-era `-1-1-1.txt` and the newest event scripts parse identically.
- New presentation features were added via **xlua hotfix patches**, not format changes: `luapatch/2060/avgcontroller.lua` implements `<下雪>` (snow) and `<火焰>` (flame) particle overlays; `luapatch/2050/avgbackgroundcontroller.lua` tweaks background rendering. This means: unknown tags degrade gracefully by design, and the tag vocabulary is discoverable from `luapatch/`.
- **Separate system**: `asset_textes/luapatch/exploredramascripts/sample*.lua` — the "ExploreDrama" 3D base-exploration cutscenes (Lua driving a Unity 3D scene: cameras, walking AI dolls). Completely different machinery. Out of scope for the VN runtime; treat as future/optional.

### 1.5 gfStory-en (the existence proof)

It already does, end to end: download live client bundles → extract with UnityPy → transpile avgtxt → replay in a web VN viewer. Components we can learn from (not copy — repo has no license file):

- `unpack/downloader.patch` + `downloader/`: fetches `.ab` bundles from the live CDN using the client's own version manifest.
- `unpack/src/gfunpack/stories.py`: the reference avgtxt parser/transpiler (500 lines).
- `unpack/src/gfunpack/characters.py`: sprite extraction — `character_*.ab` bundles, sprite chosen via **prefab indices** (`prefabs.py`: `DialoguePicDetails{path_id, alpha_path_id, scale, offset}`), exported to PNG, indexed into `characters.json`.
- `unpack/src/gfunpack/backgrounds.py`: backgrounds/CGs in `resource_avgtexture*.ab` under `assets/resources/dabao/avgtexture/<name>.png`; the ordered name list comes from `avgtxt/profiles.txt` (present in the data, e.g. `ja-JP/.../profiles.txt`: `Square_A, 树林, 机场, …, CG.1-6 无人, …`).
- `unpack/src/gfunpack/audio.py`: BGM/SE stored as CRI ACB/AWB; `audiotemplate.txt` maps names → cues; decoded with vgmstream → wav → ffmpeg → m4a; indexed into `audio.json`.

---

## 2. Story command inventory

Measured over all 52,072 lines of `en-US/asset_textes/avgtxt` (opening `<tag>` occurrences):

| Tier | Tag(s) | Count | Meaning |
|---|---|---|---|
| **Critical** | *(speaker tag)* | 45,328 | display-name override |
| Critical | `<BIN>n</BIN>` | 6,707 | switch background/CG = index into `profiles.txt` |
| Critical | `<BGM>x</BGM>` | 3,781 | music |
| Critical | `黑屏1` / `黑屏2` / `黑点1/2` | 6,357 | blank screen (persistent) / fade-in-from-black (transient) |
| Critical | `<SE1..3>x</SE…>` | 2,789 | sound effects |
| Critical | narrator `(expr)` syntax | all lines | sprites & expressions |
| **Common** | `<color>`/`<size>` rich text | 4,603+ | colored/inline-styled text |
| Common | `通讯框` | 2,952 | "comms call" framing (character in radio box) |
| Common | `边框` | 280 | letterbox/frame style |
| Common | `震屏` / `震屏3` / `shake` | 657 | screen shake |
| Common | `<分支>n</分支>` | 327 | branch gating |
| Common | `Night` | 770 | night tint class |
| Common | `白屏1/2`, `闪屏` | 290 | white flash / blink |
| **Uncommon** | `回忆`, `关闭蒙版` | 116 | memory sepia mask on/off |
| Uncommon | `睁眼` | 46 | eyes-open animation trigger |
| Uncommon | `<c>` choices | 67 | simple choice points |
| **Cosmetic/degradable** | `火花`(+close), `下雪`, `火焰*` | ~90 | particles (lua-hotfix features) |
| Cosmetic | `grey`, `同时置暗/点亮`, `拉伸`, `平移`, `立绘振动`, `cgdelay`… | <100 total | one-off polish |
| Degrade-to-choice-or-still | `<cg>`, `<r>`, `<t>`, `<va11>`, `名单`, `快跑`, `打海猫2`, `开火` | ~20 | click-CG choices, repeatable/looping choices, minigames, credit rolls |

Notably **absent from all 52k lines**: any `<voice>`, Live2D, spine, or `<video>` reference. GFL story scripts are unvoiced; Live2D lives in menus/dorms only; the handful of story videos are driven separately (`luapatch/scripts/avgvideoplayer.lua`). Voice/video support is therefore *not* blocked by the parser — there's simply nothing to parse.

**~90–95% faithful coverage** = dialogue + speakers + sprites/expressions + backgrounds/CGs (`BIN`) + BGM + SE + black/white fades + night tint + branch/choices + comms framing + shake. Everything else can start as a logged no-op.

---

## 3. Asset pipeline

All references resolve through three generated indexes (built once on PC by the importer):

| Reference in script | Source bundle | Resolution |
|---|---|---|
| Character sprite `Name(expr)` | `character_*.ab` | prefab table (`DialoguePicDetails`) maps `lower(Name)/expr` → texture path_id (+scale/offset) → PNG. Known bad entries need fixups (gfStory-en hardcodes e.g. `G36C expr7 → G36CMod expr0`, alpha-channel companion textures for `ar18`, `npc-sakura`). Index output: `characters.json` |
| `<BIN>n` background/CG | `resource_avgtexture*.ab` | n-th entry of `profiles.txt` → `avgtexture/<name>.png`. Fallback when unmapped: warn + keep previous bg. Output: `backgrounds.json` |
| `<BGM>x` / `<SE*>x` | BGM & SE ACB/AWB bundles | `audiotemplate.txt` → cue sheet → vgmstream → wav → transcode. Output: `audio.json` |

gfStory-en logs its unresolved references during a full run (`stories.py` `record_missing_audio`, `mapper.py` warnings) — i.e., automatic resolution demonstrably gets near-complete coverage with a short tail of manual fixes.

Importer-side processing for Vita (never on-device):
- images: resize to max 960 wide, pngquant;
- audio: OGG Vorbis (SDL_mixer-friendly) or AAC;
- fonts: **glyph-subset** per language from the converted corpus (pyftsubset) → tiny TTFs covering exactly the text shipped.

---

## 4. Intermediate representation

One JSON document per story unit (scene/chapter segment). Design rules: flat event list, explicit state ops (renderer keeps no GFL knowledge), asset references are opaque keys resolved via the manifest, branching via labels/jumps so the renderer needs only a program counter.

```jsonc
{
  "id": "main-c1-n1-part1",
  "category": "main",            // main | event | mod | doll | other
  "title": "Awakening",
  "language": "en-US",
  "manifest": {                  // produced by importer; also drives packaging
    "img": { "bg_005": "img/bg_005.webp", "spr_ump45_0": "img/spr_ump45_0.png" },
    "aud": { "bgm_room": "aud/bgm_room.ogg", "se_battlefield": "aud/se_battlefield.ogg" }
  },
  "events": [
    { "t": "bg",      "id": "bg_005", "transition": "fade_black" },
    { "t": "music",   "id": "bgm_room" },
    { "t": "sfx",     "id": "se_battlefield" },
    { "t": "show",    "char": "hk416", "sprite": "spr_hk416_3" },   // slot auto-assigned by stage order
    { "t": "show",    "char": "ump9",  "sprite": "spr_ump9_0" },
    { "t": "say",     "name": "UMP9", "text": ["Hey, wake up.", "416, wake up~!"],
                      "chars": [["ump9",0],["hk416",0]] },          // page-split text; chars = who is lit/on stage
    { "t": "effect",  "kind": "shake" },
    { "t": "label",   "id": "br1" },
    { "t": "choice",  "options": [
        { "text": "Move out", "setvar": "branch", "value": 1 },
        { "text": "Wait",     "setvar": "branch", "value": 2 } ] },
    { "t": "jumpif",  "var": "branch", "neq": 1, "label": "br1_skip" },
    // ... conditional block flattened from <分支> ...
    { "t": "label",   "id": "br1_skip" },
    { "t": "end" }
  ]
}
```

Notes:
- `say.chars` snapshots the visible cast + active expression per line, mirroring the original "narrator list is the stage state" semantics without the renderer parsing GFL quirks.
- Expressions stay integers at the IR boundary (that's what GFL uses); the importer maps them to concrete sprite asset keys, so a future source with named expressions changes nothing downstream.
- Unknown original tags become either dropped events (cosmetic) or `{ "t": "effect", "kind": "<tag>" }` stubs the renderer ignores — never a parse failure.
- Progress save = `{story_id, event_index, vars}`.

Skipped deliberately: a general scripting/VM layer (brocatel-style markdown+Lua). Flat JSON covers 100% of observed commands; add a scripted-event escape hatch only if a concrete scene demands it.

---

## 5. Vita architecture

**Recommendation: one portable C++ codebase — "gflvn-core" (IR loader, event stepper, save/state) over an SDL2 backend — built for desktop first, then VitaSDK.**

Vita stack: VitaSDK toolchain; SDL2 (vitasdk port, GL/vitaGL video backend), SDL2_mixer (OGG), SDL2_ttf/Freetype. Touch + buttons both come through SDL events. Save data via SDL filesystem layer → `ux0:data/gflvita/`.

Why:
- Desktop and Vita share ~100% of code → the desktop build *is* the reference player and importer test harness; no throwaway prototype.
- Memory footprint: SDL2 + freetype + a few textures at 960×544 fits trivially in 512 MB; textures streamed per scene with 1–2 ahead prefetched.
- Text/Unicode: freetype handles JP/CJK glyph rasterization; complex-shaping not needed for the target languages. Importer-side subsetting keeps fonts ~100s of KB.
- Maintainability: SDL2-on-Vita is an actively packaged, widely used path (many homebrew ports); hiring Google for answers works.

Rejected alternatives:
- **Ren'Py (incl. reusing a Ren'Py Vita/SubaHibi-style project)**: Ren'Py's Vita target died with the 7.x line; dragging a Python VM along for what is a fixed-feature reader buys nothing, and our IR would be fighting its screen/statement model anyway.
- **Raw vita2d/libvita2d without SDL**: fewer deps but loses free desktop parity — we'd write two renderers. The SDL abstraction costs almost nothing at this scale.
- **Embedding Lua/Wren/brocatel-mdc in the runtime**: an interpreter to interpret our already-flat IR. YAGNI.
- **Unity/web/everything else**: excluded by constraints and common sense.

Video (later milestone, not MVP): Vita hardware H.264 via SceAvPlayer with preconverted MP4; fallback still image where absent.

---

## 6. MVP plan

Goal slice: **Main Story Chapter 1 opening (`-1-1-1.txt` and neighbors), fully converted, playable on desktop, then on Vita.**

```
01  Parser: avgtxt line -> typed beats (Python, stdlib only).
    Test: parse -1-1-1.txt golden file; unknown-tag log must be non-fatal.
02  Asset resolver v0: hand-run UnityPy extraction for just this scene's
    referenced sprites/bg/BGM/SE (reuse gfunpack approach, our own code).
    Test: every asset key in exported JSON resolves to a real file, else warning list.
03  IR exporter: beats -> normalized scene JSON (schema §4) + manifest.
    Test: schema validation + round-trip diff of dialogue text vs source.
04  C++ core + SDL2 desktop player: bg/music/sfx/sprites/dialogue/advance.
    Test: play chapter-1 opening start-to-finish from exported JSON only.
05  Branching & choices (<c>/<分支> flattening), fades/night/shake effects.
    Test: scripted playthrough hitting both branches.
06  Full chapter 1 conversion: all node scripts, chapter tree from
    story_playback/mission tables, read/unread tracking, resume save.
    Test: every ch.1 script converts with <=N% degraded events.
07  Vita build: VitaSDK cross-compile, controls (X advance, O back,
    Triangle backlog, Square auto, R skip, Start menu), touch tap.
    Test: chapter 1 playable on hardware.
08  Reader niceties: backlog, auto mode, text speed, skip-read.
09  Widen content: events, fetter/doll stories, MOD stories via the
    existing table joins; batch import with warning reports.
```

---

## 7. Risk analysis

| Risk | Evidence | Assessment |
|---|---|---|
| **Format drift over GFL's lifetime** | 2016 and latest scripts parse identically; new features arrive as xlua hotfixes (`luapatch/2060/avgcontroller.lua`) | **Low.** Unknown-tag policy (log+drop) absorbs surprises |
| **Special-case content outside the clean pipeline** | gfStory-en needs 856 lines of `manual_chapters.py`: collab extras (VA-11, Gunslinger Girl, Sac2045, Cocoon), "attached stories", anniversary naming, blocklists | **Real work, bounded.** Metadata curation, not engine work. Expect a growing fixups table in our importer |
| **Sprite/expression mapping gaps** | `_wrong_sprites`, alpha-texture postfix lists, path_id misses warned in `mapper.py` | Medium annoyance. Mitigate by adopting the same warn-and-fallback strategy + shipping a community-editable override file |
| **Embedded minigames** | `<va11>` drink-mixing, `打海猫` cat-slapping, `<cg>` click-on-image choices | Degrade to plain choices/stills; accept the loss |
| **ExploreDrama 3D cutscenes** | `exploredramascripts/*.lua` — camera/AI choreography | Out of scope; represent as placeholder card or skip |
| **Videos** | Not referenced in scripts; separate `avgvideoplayer.lua` flow | Only affects a handful of scenes; preconvert or still-frame |
| **Live2D** | Zero occurrences in story scripts | Non-issue for the VN |
| **Localization** | Same filenames across 4 locales; text-only deltas | Easy win: locale packs share all assets |
| **Font size / memory** | Full CJK fonts are tens of MB | Importer-side glyph subsetting solves it; verify early on device (step 07) |
| **Audio formats** | CRI ACB/AWB everywhere | Solved problem (vgmstream), just pipeline plumbing |
| **Data acquisition / sunset** | Assets fetched from live CDN via client manifest; Dimbreath auto-mirrors text data | **Act now**: archive a complete bundle set + text dump once. Text data is safe (Dimbreath), art/audio exist only in client downloads |
| **PC/mobile client differences** | Same bundle CDN & manifests feed both (gfStory-en downloads the PC set) | Immaterial — importer consumes the same bundles |

---

## Appendix A — measurement script

Tag frequency counts in §2 were produced by scanning `research/GirlsFrontlineData/en-US/asset_textes/avgtxt/*.txt` with a regex counter (`<(/?)([^<>]+?)(?:=...)?>` over all lines); locale filename comparison via set intersection of directory listings. Clones retained in `research/` for further experiments.
