#!/usr/bin/env python3
"""Tests for gflvn.ir exporter. Run: python3 test_ir.py"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent / "src"))
from gflvn import ir
from gflvn.avgtxt import parse_script

ROOT = pathlib.Path(__file__).parent
SRC = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/-1-1-1.txt"
PROFILES = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/profiles.txt"


def _scene():
    doc = parse_script(SRC.read_text(encoding="utf-8"), SRC.name)
    return doc, ir.to_ir(doc, assets_profiles())


def assets_profiles():
    from gflvn.assets import load_profiles
    return load_profiles(PROFILES)


def test_schema_valid():
    _, scene = _scene()
    ir.validate(scene)
    assert scene["events"][0]["t"] == "start" and scene["events"][-1]["t"] == "end"
    types = {e["t"] for e in scene["events"]}
    assert {"bg", "music", "sfx", "show", "say"} <= types


def test_roundtrip_dialogue():
    """Every source text page appears exactly once across say events."""
    doc, scene = _scene()
    src_pages = [p.strip() for b in doc["beats"] for p in b["text"]]
    ir_pages = [p for e in scene["events"] if e["t"] == "say" for p in e["text"]]
    assert src_pages == ir_pages
    # choices carry their option texts
    opt_pages = [o for e in scene["events"] if e["t"] == "choice" for o in e["options"]]
    for b in doc["beats"]:
        for o in b.get("choice", {}).get("options", []):
            assert o.strip() in opt_pages


def test_stage_semantics():
    """First UMP9 line shows her; cast snapshots mirror narrator lists."""
    _, scene = _scene()
    shows = [e for e in scene["events"] if e["t"] == "show"]
    chars = [e["char"] for e in shows]
    assert "ump9" in chars and "hk416" in chars
    says = [e for e in scene["events"] if e["t"] == "say"]
    first_ump9 = next(s for s in says if s["name"] == "UMP9")
    assert ["ump9", 0] in first_ump9["chars"]
    narration = [s for s in says if s["name"] == ""]
    assert narration and narration[0]["chars"] == []


def test_gate_passthrough():
    doc = {"id": "t", "warnings": [], "beats": [
        {"speaker": "", "cast": [{"name": "A", "expr": None}], "text": ["gated"], "fx": {"分支": "2"}},
    ]}
    scene = ir.to_ir(doc, [])
    say = next(e for e in scene["events"] if e["t"] == "say")
    assert say["gate"] == 2


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"PASS {name}")
    print("all tests passed")
