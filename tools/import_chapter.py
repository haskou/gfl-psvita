#!/usr/bin/env python3
"""Batch import a set of story scripts (e.g. main story chapter 1).

Host-side, stdlib only:
  1. parse each matching avgtxt file -> beats
  2. export IR -> assets/scenes/<id>.ir.json
  3. discover needed prefab bundles from resdata -> download them (+ deps later in docker)

Usage: python3 tools/import_chapter.py '-1-*'
       python3 tools/import_chapter.py --manifest
       python3 tools/import_chapter.py --all
"""
import fnmatch
import importlib.util
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from gflvn.avgtxt import parse_script
from gflvn.assets import load_profiles
from gflvn.ir import to_ir, slug

ROOT = Path(__file__).resolve().parent.parent
CURRENT_DATA = ROOT / "research/gf-data-us"
AVGTXT = (CURRENT_DATA / "asset/avgtxt") if CURRENT_DATA.exists() else \
         (ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt")
PROFILES = AVGTXT / "profiles.txt"
RESDATA = ROOT / "assets/bundles/us_resdata.json"
BUNDLES = ROOT / "assets/bundles"

# narrator name -> prefab name when they differ (CJK mostly); investigation §7
NAME_OVERRIDES = {
    "赫丽安": "NPC-Helian",
    "克鲁格": "NPC-Kyruger",
    "ST AR-15": "AR15",
    "M4 SOPMOD ": "M4 SOPMOD II",
    "RO": "RO635",
}
# known no-sprite narrators (ringleaders use a different prefab system): text-only
SPRITE_GAPS = {"？？", "代理人", "梦想家", "衔尾蛇"}


def load_block_list():
    path = ROOT / "research/gfStory-en/unpack/src/gfunpack/manual_chapters.py"
    spec = importlib.util.spec_from_file_location("gfstory_manual_chapters_import", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module.get_block_list()


def scene_id_for_source(path):
    """Stable flat ID preserving the sign and source subdirectory."""
    rel = path.relative_to(AVGTXT).with_suffix("")
    if len(rel.parts) == 1:
        stem = rel.name.rstrip(".")
        return ("neg__" + stem[1:]) if stem.startswith("-") else ("main__" + stem)
    return "__".join(rel.parts).rstrip(".")


def norm(s):
    return "".join(c for c in s.lower() if c.isalnum())


def read_script(path):
    data = path.read_bytes()
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        # One current EN tip file contains two legacy single-byte punctuation
        # marks; preserve the text instead of dropping the whole chapter.
        return data.decode("latin-1")


def find_prefab_bundles(names):
    """cast names -> {prefab_bundle_names} via resdata assetAllRes pathKeys."""
    resdata = json.loads(RESDATA.read_text())
    wanted = {norm(NAME_OVERRIDES.get(n, n)) for n in names}
    found = {}
    for b in resdata["BaseAssetBundles"] + resdata["AddAssetBundles"]:
        if not b["assetBundleName"].startswith("avgpicprefabs"):
            continue
        for r in b["assetAllRes"]:
            base = r["pathKey"].rsplit("/", 1)[-1]
            stem = norm(base.rsplit(".", 1)[0])
            if stem in wanted:
                found.setdefault(stem, b["assetBundleName"])
    return found


def download(names):
    names = [n for n in names if not (BUNDLES / f"{n}.ab").exists()]
    if not names:
        return
    import subprocess
    subprocess.run([sys.executable, str(ROOT / "tools/fetch_bundles.py"), *names], check=True)


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else "-1-*"
    if pattern == "--all":
        blocked = load_block_list()
        files = sorted(
            f for f in AVGTXT.rglob("*.txt")
            if f.relative_to(AVGTXT).as_posix() not in blocked
        )
    elif pattern == "--manifest":
        wanted = set(json.loads((ROOT / "assets/manifest.json").read_text())["scenes"])
        files = sorted(
            f for f in AVGTXT.rglob("*.txt")
            if scene_id_for_source(f) in wanted
        )
        found = {scene_id_for_source(f) for f in files}
        missing = sorted(wanted - found)
        if missing:
            raise RuntimeError(f"manifest scenes without source scripts: {missing}")
    else:
        files = sorted(f for f in AVGTXT.glob("*.txt") if fnmatch.fnmatch(f.name, pattern))
    profiles = load_profiles(PROFILES)
    scenes_dir = ROOT / "assets/scenes"
    scenes_dir.mkdir(parents=True, exist_ok=True)

    cast, ids = set(), []
    for f in files:
        sid = scene_id_for_source(f)
        doc = parse_script(read_script(f), sid)
        scene = to_ir(doc, profiles)
        (scenes_dir / f"{sid}.ir.json").write_text(json.dumps(scene, ensure_ascii=False, indent=1))
        (scenes_dir / f"{sid}.beats.json").write_text(json.dumps(doc, ensure_ascii=False, indent=1))
        ids.append(sid)
        for ev in scene["events"]:
            if ev["t"] == "show":
                cast.add(ev["char"])

    if len(ids) != len(set(ids)):
        raise RuntimeError("source paths produced duplicate scene IDs")
    if pattern == "--all":
        wanted_ids = set(ids)
        for generated in scenes_dir.glob("*.ir.json"):
            if generated.name.removesuffix(".ir.json") not in wanted_ids:
                generated.unlink()
        for generated in scenes_dir.glob("*.beats.json"):
            if generated.name.removesuffix(".beats.json") not in wanted_ids:
                generated.unlink()
    source_index = {
        scene_id_for_source(f): f.relative_to(AVGTXT).as_posix()
        for f in files
    }
    (ROOT / "assets/source_index.json").write_text(
        json.dumps(source_index, ensure_ascii=False, indent=1) + "\n"
    )

    print(f"scenes: {len(ids)}")
    missing = sorted(c for c in cast if c in {slug(g) for g in SPRITE_GAPS})
    if missing:
        print(f"no-sprite narrators (text-only): {missing}")

    resolvable = [c for c in cast if c not in {slug(g) for g in SPRITE_GAPS}]
    # map slugged keys back to original names for override lookup
    orig = {}
    for f in files:
        doc = parse_script(read_script(f), "x")
        for b in doc["beats"]:
            for ch in b["cast"]:
                if ch["name"]:
                    orig.setdefault(slug(ch["name"]), ch["name"])
    bundle_map = find_prefab_bundles(orig.values())
    prefab_bundles = sorted(set(bundle_map.values()))
    def want_key(name):
        return norm(NAME_OVERRIDES.get(name, name))

    unmatched = [orig[c] for c in resolvable if want_key(orig.get(c, c)) not in bundle_map]
    if unmatched:
        print(f"WARN no prefab found for: {unmatched}")
    print(f"downloading prefab bundles: {prefab_bundles}")
    download(prefab_bundles)


if __name__ == "__main__":
    main()
