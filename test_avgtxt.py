#!/usr/bin/env python3
"""Golden + unit tests for avgtxt parser. Run: python3 test_avgtxt.py"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent / "src"))
from gflvn.avgtxt import parse_script

ROOT = pathlib.Path(__file__).parent
SRC = ROOT / "research/GirlsFrontlineData/en-US/asset_textes/avgtxt/-1-1-1.txt"
GOLDEN = ROOT / "tests/golden/-1-1-1.json"


def test_golden():
    doc = parse_script(SRC.read_text(encoding="utf-8"), SRC.name)
    expected = json.loads(GOLDEN.read_text(encoding="utf-8"))
    assert doc == expected, "parse output drifted from golden; rerun avgtxt.py --update-golden only if intended"
    # invariants from investigation.md
    assert len(doc["beats"]) == 39
    assert doc["warnings"] == []
    b0 = doc["beats"][0]
    assert b0["fx"]["bgm"] == "BGM_Room" and b0["fx"]["bin"] == "27"
    speakers = [b["speaker"] for b in doc["beats"]]
    assert speakers[1] == "Helian" and speakers[2] == "Kryuger"  # <Speaker> override


def test_choices():
    doc = parse_script(" ()||:Do you move out?<c>Move out<c>Wait here", "t.txt")
    beat = doc["beats"][0]
    assert beat["text"] == ["Do you move out?"]
    assert beat["choice"]["kind"] == "c"
    assert beat["choice"]["options"] == ["Move out", "Wait here"]
    assert doc["warnings"] == []


def test_branch_and_unknown():
    doc = parse_script(" ()||<分支>2</分支><下雪><cgdelay>:Snowy branch.", "t.txt")
    beat = doc["beats"][0]
    assert beat["fx"]["分支"] == "2"
    assert beat["fx"]["下雪"] == ""  # known flag
    assert beat["fx"]["cgdelay"] == ""  # preserved verbatim, non-fatal
    assert doc["warnings"], "unknown tag should warn but not fail"


def test_multipage_and_cast():
    line = 'HK416(2)<Speaker>416</Speaker>;UMP45(0)||<黑屏2><Night>:Page one.+Page two.'
    beat = parse_script(line, "t.txt")["beats"][0]
    assert beat["text"] == ["Page one.", "Page two."]
    assert beat["cast"] == [{"name": "HK416", "expr": 2}, {"name": "UMP45", "expr": 0}]
    assert "黑屏2" in beat["fx"] and "night" in beat["fx"]
    assert beat["speaker"] == "416"


def test_garbage_never_fatal():
    doc = parse_script("", "t.txt")
    assert doc["beats"] == []
    doc = parse_script("\nno colon here\n ()||:ok", "t.txt")
    assert len(doc["beats"]) == 1 and len(doc["warnings"]) == 1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"PASS {name}")
    print("all tests passed")
