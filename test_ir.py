#!/usr/bin/env python3
"""Tests for gflvn.ir exporter. Run: python3 test_ir.py"""
import pathlib
import sys
from unittest import SkipTest

sys.path.insert(0, str(pathlib.Path(__file__).parent / "src"))
from gflvn import ir
from gflvn.avgtxt import parse_script

ROOT = pathlib.Path(__file__).parent
SRC = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/-1-1-1.txt"
PROFILES = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/profiles.txt"


def _scene():
    if not SRC.is_file() or not PROFILES.is_file():
        raise SkipTest("requires research/GirlsFrontlineData")
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
    """Speaker-only lines stay empty; explicit expressions mirror the cast."""
    _, scene = _scene()
    shows = [e for e in scene["events"] if e["t"] == "show"]
    chars = [e["char"] for e in shows]
    assert "ump9" in chars and "hk416" in chars
    says = [e for e in scene["events"] if e["t"] == "say"]
    ump9_lines = [s for s in says if s["name"] == "UMP9"]
    assert ump9_lines[0]["chars"] == []
    assert ["ump9", 3] in next(s for s in ump9_lines if ["ump9", 3] in s["chars"])["chars"]
    narration = [s for s in says if s["name"] == ""]
    assert narration and narration[0]["chars"] == []


def test_gate_passthrough():
    doc = {"id": "t", "warnings": [], "beats": [
        {"speaker": "", "cast": [{"name": "A", "expr": None}], "text": ["gated"], "fx": {"分支": "2"}},
    ]}
    scene = ir.to_ir(doc, [])
    say = next(e for e in scene["events"] if e["t"] == "say")
    assert say["gate"] == 2


def test_remote_call_state_persists_for_the_character():
    doc = parse_script(
        "A(0)<通讯框>;B(0)||:Call.\nA(1);B(0)||:Still connected.",
        "remote.txt",
    )
    scene = ir.to_ir(doc, ["bg"])
    says = [event for event in scene["events"] if event["t"] == "say"]
    assert says[0]["stage"] == [
        {"char": "a", "expr": 0, "remote": True},
        {"char": "b", "expr": 0, "remote": False},
    ]
    assert says[1]["stage"][0]["remote"] is True


def test_choice_commits_an_absolute_stage_snapshot():
    doc = parse_script("A(1);B(0)||:Choose.<c>Left<c>Right", "choice-stage.txt")
    scene = ir.to_ir(doc, ["bg"])
    choice = next(event for event in scene["events"] if event["t"] == "choice")
    assert choice["stage"] == [
        {"char": "a", "expr": 1, "remote": False},
        {"char": "b", "expr": 0, "remote": False},
    ]


def test_cg_sequence_matches_gfstory_steps():
    doc = parse_script(" ()||<CG>0,2</CG>:After.", "cg.txt")
    scene = ir.to_ir(doc, ["a", "b", "c"])
    cg = [e for e in scene["events"] if e.get("cg")]
    assert cg == [{"t": "bg", "id": "bg_0", "cg": True},
                  {"t": "bg", "id": "bg_2", "cg": True}]
    assert [e["text"] for e in scene["events"] if e["t"] == "say"][:2] == [["……"], ["…………"]]


def test_presentation_effects_are_not_dropped():
    doc = parse_script(
        "A(0)||<BIN>0</BIN><CGDelay>0.5</CGDelay><BIN_SlowIn><白屏1><回忆><下雪>:Wake.",
        "effects.txt",
    )
    scene = ir.to_ir(doc, ["bg"])
    bg = next(event for event in scene["events"] if event["t"] == "bg")
    assert bg["transition"] == "slow_fade" and bg["delay_ms"] == 500
    kinds = {event["kind"] for event in scene["events"] if event["t"] == "effect"}
    assert {"flash_white", "memory_mask_on", "snow_on"} <= kinds


def test_empty_expression_is_speaker_only_not_expression_zero():
    doc = parse_script(
        "M4A1()<Speaker>M4A1</Speaker>||<BIN>0</BIN>:Voice only.\n"
        "M4A1(1)<Speaker>M4A1</Speaker>||:Now visible.",
        "speaker-only.txt",
    )
    scene = ir.to_ir(doc, ["bg"])
    says = [event for event in scene["events"] if event["t"] == "say"]
    assert says[0]["name"] == "M4A1"
    assert says[0]["chars"] == [] and says[0]["stage"] == []
    assert says[1]["chars"] == [["m4a1", 1]]
    shows = [event for event in scene["events"] if event["t"] == "show"]
    assert shows == [{"t": "show", "char": "m4a1", "expr": 1}]


def test_eyes_open_background_stays_covered_until_effect():
    doc = parse_script("Agent()||<BIN>0</BIN><睁眼>:Open.", "eyes.txt")
    scene = ir.to_ir(doc, ["bg"])
    bg = next(event for event in scene["events"] if event["t"] == "bg")
    assert bg["transition"] == "eyes_open"
    assert next(event for event in scene["events"] if event["t"] == "say")["stage"] == []


def test_persistent_black_after_transient_does_not_leak_new_background():
    doc = parse_script(
        "Agent()||<黑屏2><黑屏1><BIN>0</BIN>:Remain covered.",
        "covered-bin.txt",
    )
    scene = ir.to_ir(doc, ["bg"])
    kinds = [event["kind"] for event in scene["events"] if event["t"] == "effect"]
    assert kinds == ["black_on"]


def test_transient_after_persistent_still_reveals_background():
    doc = parse_script(
        "Agent()||<黑屏1><黑屏2><BIN>0</BIN>:Reveal.",
        "revealed-bin.txt",
    )
    scene = ir.to_ir(doc, ["bg"])
    kinds = [event["kind"] for event in scene["events"] if event["t"] == "effect"]
    assert kinds == ["black_on", "fade_from_black"]


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
