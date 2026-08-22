#!/usr/bin/env python3
"""Tests for gflvn.assets resolver. Run: python3 test_assets.py"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent / "src"))
from gflvn import assets
from gflvn.avgtxt import parse_script

ROOT = pathlib.Path(__file__).parent
SRC = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/-1-1-1.txt"
PROFILES = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/profiles.txt"


def test_profiles_load():
    profiles = assets.load_profiles(PROFILES)
    assert len(profiles) == 364
    assert all(p for p in profiles)


def test_resolve_real_scene():
    doc = parse_script(SRC.read_text(encoding="utf-8"), SRC.name)
    r = assets.resolve_scene(doc, assets.load_profiles(PROFILES))
    assert set(r["bg"]) == {"27", "9", "3"} and all(r["bg"].values())
    assert list(r["bgm"]) == ["BGM_Room", "BGM_Empty", "BGM_NightOPS"]
    assert "Battlefield" in r["se"] and "Alarm" in r["se"]
    sprites = [tuple(s) for s in r["sprites"]]
    assert ("ump45", 0) in sprites and ("hk416", 2) in sprites
    assert r["warnings"] == []


def test_out_of_range_bin_warns():
    doc = {"beats": [{"fx": {"bin": "999"}, "cast": []}], "warnings": []}
    r = assets.resolve_scene(doc, ["a", "b"])
    assert len(r["warnings"]) == 1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"PASS {name}")
    print("all tests passed")
