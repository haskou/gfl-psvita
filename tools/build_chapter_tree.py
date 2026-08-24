#!/usr/bin/env python3
"""Build the gfStory-en story-selection hierarchy for the imported scenes.

This deliberately reuses gfStory-en's chapter fixups instead of inferring a
chapter from a filename.  The latter loses campaign signs and flattens stages.
"""

from __future__ import annotations

import dataclasses
import importlib.util
import json
import re
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GFSTORY = ROOT / "research/gfStory-en"
CURRENT_DATA = ROOT / "research/gf-data-us"
GFDATA = CURRENT_DATA if CURRENT_DATA.exists() else ROOT / "research/GirlsFrontlineData/en-US"


def load_manual_module():
    path = GFSTORY / "unpack/src/gfunpack/manual_chapters.py"
    spec = importlib.util.spec_from_file_location("gfstory_manual_chapters", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def translations(name: str) -> dict[str, str]:
    result: dict[str, str] = {}
    table_root = GFDATA / ("asset/table" if (GFDATA / "asset/table").exists() else "asset_textes/table")
    path = table_root / f"{name}.txt"
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition(",")
        if sep:
            result[key] = value.replace("//c", ",")
    return result


def stc_data(name: str):
    legacy = GFDATA / f"stc/{name}.json"
    current = GFDATA / f"stc/{name.removeprefix('Stc').lower()}.json"
    return json.loads((current if current.exists() else legacy).read_text())


def story_playback() -> list[dict[str, object]]:
    """Parse the small HJSON file without adding a build-time dependency."""
    # gfStory-en's fixed table keeps the official English chapter subtitles;
    # the current client table regressed these to generic "Chapter N" labels.
    path = GFSTORY / "unpack/fixed-data/formatted/story_playback.hjson"
    text = path.read_text()
    items: list[dict[str, object]] = []
    for body in re.findall(r"\{([^{}]*)\}", text, re.S):
        item: dict[str, object] = {}
        for raw in body.splitlines():
            line = raw.strip()
            if not line or ":" not in line:
                continue
            key, value = line.split(":", 1)
            value = value.strip().strip('"')
            item[key.strip()] = int(value) if re.fullmatch(r"-?\d+", value) else value
        if "id" in item and "name" in item:
            items.append(item)
    return items


def event_files(event: dict[str, object]) -> list[str]:
    files: list[str] = []
    scripts = [s.strip() for s in str(event.get("scripts", "")).split(",") if s.strip()]
    for key in ("first", "start"):
        value = str(event.get(key, "")).strip()
        if value and value not in scripts:
            scripts.insert(0, value)
    for key in ("point", "step_start_story", "round"):
        for value in str(event.get(key, "")).split(","):
            if ":" in value:
                script = value.split(":", 1)[1].strip()
                if script and script not in scripts:
                    scripts.append(script)
    end = str(event.get("end", "")).strip()
    if end and end not in scripts:
        scripts.append(end)
    for script in scripts:
        name = f"{script.lower()}.txt"
        if name not in files:
            files.append(name)
    return files


def main() -> None:
    manual = load_manual_module()
    Story, Chapter = manual.Story, manual.Chapter
    available_ids = {p.name.removesuffix(".ir.json") for p in (ROOT / "assets/scenes").glob("*.ir.json")}
    source_index = json.loads((ROOT / "assets/source_index.json").read_text())
    source_index = {scene: raw for scene, raw in source_index.items() if scene in available_ids}
    source_to_id = {raw: scene for scene, raw in source_index.items()}
    extracted = set(source_to_id)

    def scene_for(raw: str) -> str | None:
        return source_to_id.get(raw)

    chapters, campaign_to_chapter, mapped = manual.get_recorded_chapters()
    for info in story_playback():
        chapter_id = int(info["id"])
        if chapter_id not in chapters:
            tag = str(info.get("tag", ""))
            number = str(info.get("chapter", "0"))
            prefix = "" if not tag else f"{tag} {number} "
            chapters[chapter_id] = Chapter(prefix + str(info["name"]), number, [])
        for campaign in str(info.get("story_campaign_id", "0")).split(","):
            campaign_to_chapter[campaign] = chapter_id

    # The in-game chronicle table stops at Fixed Point even though the current
    # EN client ships all later scripts. Keep the post-chronicle archive explicit
    # (matching gfStory's deployed chapter tree) so these campaigns never fall
    # into a generic bucket or get merged with the wrong event.
    later_chapters = [
        ("EP. 13.75 Poincare Recurrence", "13.75", "", (-48,)),
        ("EP. 13.8 Fixed Point", "13.8", "", (-51,)),
        ("EP. 13.9 Longitudinal Strain", "13.9", "", (-54,)),
        ("EP. 13.95 Eclipses & Saros", "13.95", "", (-56,)),
        ("EP. 14 Slow Shock", "14", "", (-58, -99)),
        ("EP. 15.1 The Summer Garden of Forking Paths", "15.1", "", (-68,)),
        ("EP. 15.2 Cartesian Theater", "15.2", "", (-69,)),
        ("EP. 15.3 Zero Charge", "15.3", "", (-70,)),
        ("EP. 15.4 Angular Gyrus", "15.4", "", (-71,)),
        ("EP. 15.5 Isolation Forest", "15.5", "", (-72,)),
        ("EP. 15.6 Convolutional Kernel", "15.6", "", (-74,)),
        ("EP. 15.7 Roche Limit", "15.7", "", (-75,)),
        ("EP. 15.8 Virtual Pair", "15.8", "", (-76,)),
        ("EP. 15.9 Quantum Fluctuation", "15.9", "", (-77, -78)),
        ("C.E. 2021 One Coin Short", "2021", "2021", (-49,)),
        ("C.E. 2021 The Waves Wrangler", "2021", "2021", (-47,)),
        ("C.E. 2022 Lycan Sanctuary", "2022", "2022", (-52,)),
        ("C.E. 2022 Love Bakery", "2022", "2022", (-50,)),
        ("C.E. 2023 Lost in Thoughts", "2023", "2023", (-61,)),
        ("C.E. 2023 Permitted! Reloading", "2023", "2023", (-62,)),
        ("C.E. 2023 Maze Guess", "2023", "2023", (-59,)),
        ("C.E. 2024 The Empty House", "2024", "2024", (-66,)),
        ("C.E. 2024 Island Getaway", "2024", "2024", (-67,)),
        ("C.E. 2025 Fireworks Broadcast", "2025", "2025", (-79,)),
        ("Silent Sandbox", "", "2024", ()),
        ("My Devil's Frontline", "", "collab", (-46,)),
        ("The Glistening Bloom", "", "collab", (-57,)),
        ("Through the Looking-Glass", "", "collab", (-64,)),
        ("Lorenz Butterfly", "", "collab", (-73,)),
    ]
    later_ids: dict[str, int] = {}
    for offset, (name, number, description, campaigns) in enumerate(later_chapters):
        existing = next((campaign_to_chapter.get(str(c)) for c in campaigns
                         if str(c) in campaign_to_chapter), None)
        chapter_id = existing if existing is not None else 40000 + offset
        if chapter_id not in chapters:
            chapters[chapter_id] = Chapter(name, description or number, [])
        else:
            chapters[chapter_id].name = name
            chapters[chapter_id].description = description or number
        later_ids[name] = chapter_id
        for campaign in campaigns:
            campaign_to_chapter[str(campaign)] = chapter_id

    # -77 contains both the final main campaign and a separate side event.
    # gfStory's deployed archive provides the exact file-level split.
    silent_sandbox_files: set[str] = set()
    deployed_tree = GFDATA / "gfstory_chapters.json"
    if deployed_tree.exists():
        archive = json.loads(deployed_tree.read_text())
        for chapter in archive.get("event", []):
            if "静默沙盘" not in chapter.get("name", ""):
                continue
            for story in chapter.get("stories", []):
                for file in story.get("files", []):
                    silent_sandbox_files.add(file if isinstance(file, str) else file[0])
    manual.add_extra_chapter_mappings(campaign_to_chapter)

    story_text = translations("story_util")
    mission_text = translations("mission")
    missions = stc_data("StcMission")
    mission_names = {
        int(m["id"]): mission_text.get(str(m.get("name", "")), str(m.get("name", "")))
        for m in missions
    }
    events = stc_data("StcStory_util")
    blocked = manual.get_block_list()
    for event in sorted(events, key=lambda e: (abs(int(e["campaign"])), int(e["id"]))):
        files = []
        for file in event_files(event):
            if file in mapped:
                continue
            mapped.add(file)
            if file in extracted and not manual.is_manual_processed(file) and file not in blocked:
                files.append(file)
        if not files:
            continue
        title = story_text.get(str(event.get("title", "")), str(event.get("title", "")))
        description = story_text.get(str(event.get("description", "")), str(event.get("description", "")))
        campaign = int(event["campaign"])
        if campaign <= -68:
            title = mission_names.get(int(event.get("mission_id", 0)), title)
            description = ""
        story = Story(
            name=files[0] if not title else title,
            description=manual.chapter_difficulty(int(event["id"]), description),
            files=files,
        )
        manual.manual_naming(story, campaign)
        campaign_key = str(campaign)
        forced_chapter = (later_ids["Silent Sandbox"]
                          if any(file in silent_sandbox_files for file in files) else None)
        if campaign_key not in campaign_to_chapter:
            chapter_id = 20000 + abs(campaign)
            chapters[chapter_id] = Chapter(f"Unknown: {title}", "", [])
            campaign_to_chapter[campaign_key] = chapter_id
        chapters[forced_chapter if forced_chapter is not None else campaign_to_chapter[campaign_key]].stories.append(story)

    # Seed any top-level scripts absent from the current playback table before
    # applying gfStory's attachments: several curated chains attach to one of
    # these files (for example the White Day extras under -50-1-4).
    assigned = {
        scene_for(f if isinstance(f, str) else f[0])
        for chapter in chapters.values() for story in chapter.stories for f in story.files
    }
    assigned.discard(None)
    blocked = manual.get_block_list()
    attached_targets = {
        item[1] for item in manual._attached_stories
    } | {
        (f if isinstance(f, str) else f[0])
        for _, story in manual._attached_events for f in story.files
    }
    for raw in sorted(extracted):
        scene = scene_for(raw)
        if scene in assigned or raw in blocked or raw in attached_targets or "/" in raw:
            continue
        match = re.match(r"^(-?\d+)-", raw)
        if not match:
            continue
        campaign = match.group(1)
        chapter_id = (later_ids["Silent Sandbox"] if raw in silent_sandbox_files
                      else campaign_to_chapter.get(campaign))
        if chapter_id is None:
            chapter_id = 30000 + abs(int(campaign))
            chapters[chapter_id] = Chapter(f"Other Campaign {campaign}", "", [])
            campaign_to_chapter[campaign] = chapter_id
        chapters[chapter_id].stories.append(Story(raw, "", [raw]))
        mapped.add(raw)

    # With the complete source set available, use gfStory's attachment and
    # collaboration regrouping verbatim instead of the old partial workaround.
    reachable = {
        (f if isinstance(f, str) else f[0])
        for chapter in chapters.values() for story in chapter.stories for f in story.files
    }
    usable_attachments = []
    for attachment in manual._attached_stories:
        base, attached = attachment[:2]
        if base in reachable and attached in extracted:
            usable_attachments.append(attachment)
            reachable.add(attached)
    manual._attached_stories = usable_attachments
    manual._attached_events = [
        (base, story) for base, story in manual._attached_events
        if base in reachable and all(
            (f if isinstance(f, str) else f[0]) in extracted for f in story.files
        )
    ]
    manual.post_insert(chapters, mapped)
    saga_names = {
        'Keep Clear!', 'Precious Treasure', 'Heart of the Cherry Blossom',
        'Rampaging Radio Show!', 'Rampaging Memories!', 'Rampage at the Graduation Ceremony',
        'Pavilion in the Rain', 'Moonlit Ocean', 'Dazzling Love',
        'Strangers in a Foreign Land', 'When Did the Moon Start Shining?', 'Drifting Apart in the Haze',
        'Traveler in Time', 'Sound of the Sea', 'Until the Sun Goes Down',
        'Perfect Strangers', 'Top Secret Agent', 'The Remains of the Day',
        'Night-Time Fantasia', 'Gift Giver', '"Please Don\'t Go"',
    }
    saga_chapter = chapters.get(campaign_to_chapter.get('-57'))
    if saga_chapter and saga_names <= {story.name for story in saga_chapter.stories}:
        manual.manually_process(chapters, campaign_to_chapter, mapped)

    main_chapters: list[Chapter] = []
    event_chapters: list[Chapter] = []
    colab_chapters: list[Chapter] = []
    for _, chapter in sorted(chapters.items()):
        if not any(scene_for(f if isinstance(f, str) else f[0]) in available_ids
                   for story in chapter.stories for f in story.files):
            continue
        if str(chapter.description).isdigit() and 2000 < int(chapter.description) < 2100:
            event_chapters.append(chapter)
        elif "collab" in str(chapter.description):
            colab_chapters.append(chapter)
        else:
            main_chapters.append(chapter)
    event_chapters.sort(key=lambda c: int(c.description))
    manual.fill_in_chapter_info(main_chapters, event_chapters)
    later_main_order = [name for name, _, description, campaigns in later_chapters
                        if not description and campaigns]
    later_main = {chapter.name: chapter for chapter in main_chapters if chapter.name in later_main_order}
    if later_main:
        main_chapters = [chapter for chapter in main_chapters if chapter.name not in later_main]
        main_chapters.extend(later_main[name] for name in later_main_order if name in later_main)

    def visual_key(name: str) -> str | None:
        clean = re.sub(r"^(?:EP\.|C\.E\.)\s*[0-9.]+\s*", "", name).strip()
        clean = unicodedata.normalize("NFKD", clean).encode("ascii", "ignore").decode().casefold()
        key = re.sub(r"[^a-z0-9]+", "_", clean).strip("_")
        aliases = {
            "blindfold_theorem": "operation_cube_plus",
            "only_master": "hitoribocchi",
            "dream_theater": "dream_theatre",
        }
        key = aliases.get(key, key)
        logo = ROOT / f"assets/img/event_{key}_logo.png"
        poster = ROOT / f"assets/img/event_{key}_poster.jpg"
        return key if logo.exists() and poster.exists() else None

    def encode(chapter: Chapter) -> dict[str, object]:
        stories = []
        for story in chapter.stories:
            files = []
            for i, file in enumerate(story.files):
                raw, label = (file, f"Part {i + 1}") if isinstance(file, str) else file
                scene = scene_for(raw)
                if scene in available_ids:
                    files.append({"id": scene, "label": label})
            if files:
                stories.append({"name": story.name, "description": story.description, "files": files})
        result = {"name": chapter.name, "description": chapter.description, "stories": stories}
        key = visual_key(chapter.name)
        if key:
            result["logo"] = f"event_{key}_logo"
            result["wallpaper"] = f"event_{key}_poster"
        return result

    # Non-campaign stories live in dedicated source directories. Build the
    # same high-level groups as gfStory so every imported script is reachable.
    used_raw = {
        (f if isinstance(f, str) else f[0])
        for chapter in chapters.values() for story in chapter.stories for f in story.files
        if (f if isinstance(f, str) else f[0]) in extracted
    }
    gun_rows = stc_data("StcGun")
    npc_rows = stc_data("StcNpc")
    sangvis_rows = stc_data("StcSangvis")
    skin_rows = stc_data("StcSkin")
    gun_text, npc_text = translations("gun"), translations("npc")
    sangvis_text, skin_text = translations("sangvis"), translations("skin")
    gun_names = {int(x["id"]): gun_text.get(str(x.get("name", "")), x.get("en_name", str(x["id"]))) for x in gun_rows}
    npc_names = {int(x["id"]): npc_text.get(str(x.get("name", "")), x.get("code", str(x["id"]))) for x in npc_rows}
    sangvis_names = {int(x["id"]): sangvis_text.get(str(x.get("name", "")), x.get("en_name", str(x["id"]))) for x in sangvis_rows}

    def numeric_key(value: str):
        return tuple((0, int(x)) if x.isdigit() else (1, x.casefold()) for x in re.split(r"(\d+)", value))

    bonding: list[Chapter] = []
    fetter_text, fetter_story_text = translations("fetter"), translations("fetter_story")
    fetters = {int(x["id"]): x for x in stc_data("StcFetter")}
    fetter_stories = {int(x["id"]): x for x in stc_data("StcFetter_story")}
    for fid in sorted({int(raw.split("/")[1]) for raw in extracted if raw.startswith("fetter/")}):
        row = fetters.get(fid, {})
        chapter = Chapter(fetter_text.get(str(row.get("name", "")), f"Griffin Memory {fid}"), "", [])
        raws = sorted((r for r in extracted if r.startswith(f"fetter/{fid}/")), key=numeric_key)
        for raw in raws:
            sid = int(Path(raw).stem)
            info = fetter_stories.get(sid, {})
            name = fetter_story_text.get(str(info.get("name", "")), f"Part {len(chapter.stories) + 1}")
            desc = fetter_story_text.get(str(info.get("description", "")), "")
            chapter.stories.append(Story(name, desc, [raw]))
            used_raw.add(raw)
        bonding.append(chapter)

    upgrading: list[Chapter] = []
    memoir_groups: dict[int, list[str]] = {}
    for raw in extracted:
        if raw.startswith("memoir/"):
            try: gun_id = int(Path(raw).stem.split("_", 1)[0])
            except ValueError: gun_id = 0
            memoir_groups.setdefault(gun_id, []).append(raw)
    for gun_id, raws in sorted(memoir_groups.items()):
        chapter = Chapter(gun_names.get(gun_id, f"Doll {gun_id}"), "", [])
        for i, raw in enumerate(sorted(raws, key=numeric_key)):
            chapter.stories.append(Story(f"Part {i + 1}", "Neural Upgrade", [raw]))
            used_raw.add(raw)
        upgrading.append(chapter)

    skins: list[Chapter] = []
    skin_by_id = {int(x["id"]): x for x in skin_rows}
    skin_groups: dict[int, list[str]] = {}
    for raw in extracted:
        if raw.startswith("skin/"):
            info = skin_by_id.get(int(Path(raw).stem), {})
            skin_groups.setdefault(int(info.get("fit_gun", 0)), []).append(raw)
    for gun_id, raws in sorted(skin_groups.items()):
        chapter = Chapter(gun_names.get(gun_id, npc_names.get(gun_id, f"Character {gun_id}")), "", [])
        for raw in sorted(raws, key=numeric_key):
            info = skin_by_id.get(int(Path(raw).stem), {})
            name = skin_text.get(str(info.get("name", "")), f"Skin {Path(raw).stem}")
            desc = skin_text.get(str(info.get("dialog", "")), "")
            chapter.stories.append(Story(name, desc, [raw]))
            used_raw.add(raw)
        skins.append(chapter)

    anniversary_groups: dict[str, Chapter] = {
        "Dolls": Chapter("Dolls", "", []),
        "Humans": Chapter("Humans", "", []),
        "Sangvis Ferri": Chapter("Sangvis Ferri", "", []),
        "Other": Chapter("Other", "", []),
    }
    for raw in sorted((r for r in extracted if r.startswith("anniversary/")), key=numeric_key):
        stem = Path(raw).stem.removeprefix("default_")
        group, name = "Other", stem
        if stem.lstrip("-").isdigit():
            value = int(stem)
            if value > 0 and value in gun_names: group, name = "Dolls", gun_names[value]
            elif value < 0 and value in npc_names: group, name = "Humans", npc_names[value]
        elif stem.startswith("s_") and stem[2:].isdigit():
            value = int(stem[2:])
            if value in sangvis_names: group, name = "Sangvis Ferri", sangvis_names[value]
        anniversary_groups[group].stories.append(Story(name, "", [raw]))
        used_raw.add(raw)
    anniversary = [c for c in anniversary_groups.values() if c.stories]

    additional: list[Chapter] = []
    remaining_by_dir: dict[str, list[str]] = {}
    for raw in sorted(extracted - used_raw):
        directory = raw.split("/", 1)[0] if "/" in raw else "Other"
        remaining_by_dir.setdefault(directory, []).append(raw)
    for directory, raws in sorted(remaining_by_dir.items()):
        chapter = Chapter(directory.replace("_", " ").title(), "", [])
        for raw in sorted(raws, key=numeric_key):
            chapter.stories.append(Story(Path(raw).stem, "", [raw]))
        additional.append(chapter)

    categories = []
    for name, values in (("Main Story", main_chapters), ("Minor Events", event_chapters),
                         ("Collab Events", colab_chapters), ("Griffin Memories", bonding),
                         ("Neural Upgrades", upgrading), ("Anniversary", anniversary),
                         ("Skin Stories", skins), ("Additional Stories", additional)):
        encoded = [encode(c) for c in values]
        encoded = [c for c in encoded if c["stories"]]
        if encoded:
            categories.append({"name": name, "chapters": encoded})
    output = {"categories": categories}
    path = ROOT / "assets/chapters.json"
    path.write_text(json.dumps(output, ensure_ascii=False, indent=1) + "\n")
    tree_ids = [f["id"] for c in categories for ch in c["chapters"] for s in ch["stories"] for f in s["files"]]
    count = len(tree_ids)
    missing = available_ids - set(tree_ids)
    duplicates = sorted({scene for scene in tree_ids if tree_ids.count(scene) > 1})
    if missing or duplicates or set(tree_ids) != available_ids:
        raise SystemExit(
            f"chapter tree invalid: {count}/{len(available_ids)} entries; "
            f"missing={sorted(missing)[:20]}; duplicates={duplicates[:20]}"
        )
    print(f"chapter tree: {len(categories)} categories, {count} scenes")


if __name__ == "__main__":
    main()
