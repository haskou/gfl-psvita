"""Step 03 IR exporter: typed beats -> flat event-stream scene JSON.

Schema follows docs/investigation.md section 4: renderer keeps no GFL knowledge;
asset refs are opaque keys resolved later; <分支> gating rides along as a `gate`
field until step 05 flattens it into label/jumpif pairs.
"""
from pathlib import Path

from .assets import resolve_scene


def slug(name: str) -> str:
    """ASCII-safe asset key component: lowercase alnum kept, rest hex-escaped."""
    out = []
    for ch in name.lower():
        if ch.isascii() and (ch.isalnum() or ch == "_"):
            out.append(ch)
        else:
            out.append("u" + ch.encode("utf-8").hex())
    return "".join(out)


def _speaker(beat):
    """Display override > first-listed cast member > narration."""
    if beat["speaker"]:
        return beat["speaker"]
    if beat["cast"] and beat["cast"][0]["name"]:
        return beat["cast"][0]["name"]
    return ""


def _stage_diff(prev_cast, cast):
    """Cast lists are absolute stage state -> show/hide diffs."""
    prev = {(c["name"].lower(), c["expr"] or 0) for c in prev_cast if c["name"]}
    cur = {(c["name"].lower(), c["expr"] or 0) for c in cast if c["name"]}
    events = []
    for who in prev - cur:
        events.append({"t": "hide", "char": slug(who[0])})
    for who in cur - prev:
        events.append({"t": "show", "char": slug(who[0]), "expr": who[1]})
    return events


# GFL tag -> renderer effect kind. Unmapped tags stay in beat.fx and are ignored.
_EFFECT_MAP = {
    "黑屏1": "black_on",        # persistent blank until next bg
    "黑点1": "black_on",
    "黑屏2": "fade_from_black",  # transient
    "黑点2": "fade_from_black",
    "白屏2": "flash_white",
    "闪屏": "blink",
    "震屏": "shake",
    "震屏3": "shake",
    "shake": "shake",
    "睁眼": "eyes_open",
    "回忆": "memory_mask_on",
    "关闭蒙版": "masks_off",
}

_SE_KEYS = ("se", "se1", "se2", "se3")


def to_ir(doc: dict, profiles: list[str]) -> dict:
    assets = resolve_scene(doc, profiles)
    events = [{"t": "start"}]
    prev_cast = []
    for beat in doc["beats"]:
        fx = beat["fx"]

        # --- state ops from effects ---
        if "bin" in fx:
            ev = {"t": "bg", "id": f"bg_{fx['bin']}"}
            # black_on right before a bg switch means fade-in-from-black
            if any(fx.get(k) is not None for k in ("黑屏2", "黑点2")):
                ev["transition"] = "fade_black"
            events.append(ev)
        if fx.get("bgm"):
            events.append({"t": "music", "id": f"bgm_{fx['bgm']}"})
        for key in _SE_KEYS:
            if fx.get(key):
                events.append({"t": "sfx", "id": f"se_{fx[key]}"})
        if fx.get("night") is not None:
            events.append({"t": "night", "on": True})
        for tag, val in fx.items():
            if tag in _EFFECT_MAP:
                events.append({"t": "effect", "kind": _EFFECT_MAP[tag]})

        # --- stage updates ---
        events += _stage_diff(prev_cast, beat["cast"])
        prev_cast = beat["cast"]

        # --- dialogue / choices ---
        if beat.get("choice"):
            events.append({
                "t": "choice",
                "kind": beat["choice"]["kind"],
                "options": [o.strip() for o in beat["choice"]["options"]],
            })
        elif beat["text"]:
            say = {
                "t": "say",
                "name": _speaker(beat),
                "text": beat["text"],
                "chars": [[slug(c["name"]), c["expr"] or 0] for c in beat["cast"] if c["name"]],
            }
            if fx.get("分支"):
                say["gate"] = int(fx["分支"])  # step 05 flattens this into jumpif
            events.append(say)

    return {
        "id": doc["id"],
        "language": "en-US",
        "events": events + [{"t": "end"}],
        "assets": assets,
    }


def validate(scene: dict):
    """Minimal schema check: every event has known t and required fields."""
    required = {
        "start": (), "end": (),
        "bg": ("id",), "music": ("id",), "sfx": ("id",),
        "show": ("char", "expr"), "hide": ("char",),
        "say": ("name", "text"), "choice": ("options",), "effect": ("kind",),
        "night": ("on",),
    }
    for i, ev in enumerate(scene["events"]):
        assert ev["t"] in required, f"event {i}: unknown type {ev['t']!r}"
        for field in required[ev["t"]]:
            assert field in ev, f"event {i} ({ev['t']}): missing {field!r}"
