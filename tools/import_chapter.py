#!/usr/bin/env python3
"""Batch import a set of story scripts (e.g. main story chapter 1).

Host-side, stdlib only:
  1. parse each matching avgtxt file -> beats
  2. export IR -> assets/scenes/<id>.ir.json
  3. discover needed prefab bundles from resdata -> download them (+ deps later in docker)

Usage: python3 tools/import_chapter.py '-1-*'
"""
import fnmatch
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from gflvn.avgtxt import parse_script
from gflvn.assets import load_profiles
from gflvn.ir import to_ir, slug

ROOT = Path(__file__).resolve().parent.parent
AVGTXT = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt"
PROFILES = AVGTXT / "profiles.txt"
RESDATA = ROOT / "assets/bundles/us_resdata.json"
BUNDLES = ROOT / "assets/bundles"

# narrator name -> prefab name when they differ (CJK mostly); investigation §7
NAME_OVERRIDES = {
    "赫丽安": "NPC-Helian",
    "克鲁格": "NPC-Kyruger",
}
# known no-sprite narrators (ringleaders use a different prefab system): text-only
SPRITE_GAPS = {"？？", "代理人", "梦想家", "衔尾蛇"}


def norm(s):
    return slug(s).replace("_", "")


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
    files = sorted(f for f in AVGTXT.glob("*.txt") if fnmatch.fnmatch(f.name, pattern))
    profiles = load_profiles(PROFILES)
    scenes_dir = ROOT / "assets/scenes"
    scenes_dir.mkdir(parents=True, exist_ok=True)

    cast, ids = set(), []
    for f in files:
        sid = f.stem.lstrip("-").rstrip(".")  # '-1-2-2first.txt' -> '1-2-2first'
        doc = parse_script(f.read_text(encoding="utf-8"), sid)
        scene = to_ir(doc, profiles)
        (scenes_dir / f"{sid}.ir.json").write_text(json.dumps(scene, ensure_ascii=False, indent=1))
        (scenes_dir / f"{sid}.beats.json").write_text(json.dumps(doc, ensure_ascii=False, indent=1))
        ids.append(sid)
        for ev in scene["events"]:
            if ev["t"] == "show":
                cast.add(ev["char"])

    print(f"scenes: {ids}")
    missing = sorted(c for c in cast if c in {slug(g) for g in SPRITE_GAPS})
    if missing:
        print(f"no-sprite narrators (text-only): {missing}")

    resolvable = [c for c in cast if c not in {slug(g) for g in SPRITE_GAPS}]
    # map slugged keys back to original names for override lookup
    orig = {}
    for f in files:
        doc = parse_script(f.read_text(encoding="utf-8"), "x")
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
