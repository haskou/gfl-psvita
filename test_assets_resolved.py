#!/usr/bin/env python3
"""Step 02b acceptance: every IR asset key resolves to a real file via manifest.
Run: python3 test_assets_resolved.py"""
import json
import pathlib

ROOT = pathlib.Path(__file__).parent
SCENE = "1-1-1"


def test_manifest_complete():
    scene = json.loads((ROOT / f"assets/{SCENE}.ir.json").read_text())
    manifest = json.loads((ROOT / "assets/manifest.json").read_text())

    def key_of(ev_id):
        return ev_id.replace("bgm_", "bgm_", 1)

    resolved = {**manifest["img"], **manifest["aud"]}
    checked = set()
    for ev in scene["events"]:
        if ev["t"] in ("bg", "music", "sfx"):
            assert ev["id"] in resolved, f"{ev['id']} not in manifest"
            path = ROOT / resolved[ev["id"]]
            assert path.is_file() and path.stat().st_size > 0, f"{ev['id']} -> {path} missing/empty"
            checked.add(ev["id"])
    # sprites referenced by show events
    for ev in scene["events"]:
        if ev["t"] == "show":
            key = f"spr_{ev['char']}_{ev['expr']}"
            assert key in resolved, f"{key} not in manifest"
            assert (ROOT / resolved[key]).is_file(), f"{key} file missing"
            checked.add(key)
    print(f"  ({len(checked)} asset keys verified on disk)")


if __name__ == "__main__":
    test_manifest_complete()
    print("PASS test_manifest_complete")
    print("all tests passed")
