#!/usr/bin/env python3
import json
from pathlib import Path
from unittest import SkipTest


ROOT = Path(__file__).resolve().parent


def test_gfstory_hierarchy_covers_every_scene_once():
    if not (ROOT / "assets/chapters.json").is_file():
        raise SkipTest("requires a complete local assets import")
    tree = json.loads((ROOT / "assets/chapters.json").read_text())
    assert [c["name"] for c in tree["categories"]][:3] == [
        "Main Story", "Minor Events", "Collab Events",
    ]
    files = [
        file["id"]
        for category in tree["categories"]
        for chapter in category["chapters"]
        for story in chapter["stories"]
        for file in story["files"]
    ]
    scenes = {p.name.removesuffix(".ir.json") for p in (ROOT / "assets/scenes").glob("*.ir.json")}
    assert len(files) == len(set(files)), "a scene occurs more than once in the menu"
    assert set(files) == scenes
    assert len(files) == 3582
    assert "neg__39-ex1-4-point91502" not in files
    assert all("uncategorized" not in chapter["name"].casefold()
               for category in tree["categories"] for chapter in category["chapters"])

    main = next(c for c in tree["categories"] if c["name"] == "Main Story")
    ep1 = next(c for c in main["chapters"] if c["name"] == "EP. 1 Awakening")
    cube = next(c for c in main["chapters"] if c["name"] == "EP. 5.5 Operation Cube")
    ep1_ids = [f["id"] for s in ep1["stories"] for f in s["files"]]
    cube_ids = [f["id"] for s in cube["stories"] for f in s["files"]]
    assert any(i.startswith("main__1-") for i in ep1_ids)
    assert not any(i.startswith("neg__1-") for i in ep1_ids)
    assert cube_ids and all(i.startswith("neg__1-") for i in cube_ids)
    assert cube["logo"] == "event_operation_cube_logo"
    assert cube["wallpaper"] == "event_operation_cube_poster"
    assert next(c for c in main["chapters"] if c["name"] == "EP. 15.9 Quantum Fluctuation")
    assert all("other campaign" not in chapter["name"].casefold()
               for category in tree["categories"] for chapter in category["chapters"])


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
