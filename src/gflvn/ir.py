"""Step 03 IR exporter: typed beats -> flat event-stream scene JSON.

Schema follows docs/investigation.md section 4: renderer keeps no GFL knowledge;
asset refs are opaque keys resolved later; <分支> gating rides along as a `gate`
field until step 05 flattens it into label/jumpif pairs.
"""
from pathlib import Path

from .assets import resolve_scene


def slug(name: str) -> str:
    """ASCII-safe asset key component: lowercase alnum plus -_ kept, rest hex-escaped."""
    out = []
    for ch in name.lower():
        if ch.isascii() and (ch.isalnum() or ch in "-_"):
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


def _visible_cast(cast):
    """Characters with an explicit expression are the only visible sprites.

    GFL scripts use ``Name()`` for a speaker-only line.  Treating its missing
    expression as expression 0 invents character art that the original scene
    never displays.
    """
    return [c for c in cast if c["name"] and c.get("expr") is not None]


def _stage_diff(prev_cast, cast):
    """Cast lists are absolute stage state -> show/hide diffs."""
    prev = {(c["name"].lower(), c["expr"]) for c in _visible_cast(prev_cast)}
    cur = {(c["name"].lower(), c["expr"]) for c in _visible_cast(cast)}
    events = []
    for who in prev - cur:
        events.append({"t": "hide", "char": slug(who[0])})
    for who in cur - prev:
        events.append({"t": "show", "char": slug(who[0]), "expr": who[1]})
    return events


def _stage_snapshot(cast, remote_chars):
    """Absolute stage state consumed atomically at a visible text/choice boundary."""
    return [
        {
            "char": slug(c["name"]),
            "expr": c["expr"],
            "remote": c["name"].lower() in remote_chars,
            **({"effect": "stealth"} if c.get("mod") == "隐身" else {}),
        }
        for c in _visible_cast(cast)
    ]


# GFL tag -> renderer effect kind. Unmapped tags stay in beat.fx and are ignored.
_EFFECT_MAP = {
    "黑屏1": "black_on",        # persistent blank until next bg
    "黑点1": "black_on",
    "黑屏2": "fade_from_black",  # transient
    "黑点2": "fade_from_black",
    "白屏2": "flash_white",
    "白屏1": "flash_white",
    "白屏闪光": "flash_white",
    "闪屏": "blink",
    "震屏": "shake",
    "震屏3": "shake",
    "shake": "shake",
    "睁眼": "eyes_open",
    "回忆": "memory_mask_on",
    "关闭蒙版": "masks_off",
    "火花": "sparks_on",
    "关闭火花": "sparks_off",
    "火花关闭": "sparks_off",
    "下雪": "snow_on",
    "火焰": "flames_on",
    "火焰销毁": "particles_off",
}

_SE_KEYS = ("se", "se1", "se2", "se3")


def to_ir(doc: dict, profiles: list[str]) -> dict:
    assets = resolve_scene(doc, profiles)
    events = [{"t": "start"}]
    prev_cast = []
    remote_chars = set()
    for beat in doc["beats"]:
        fx = beat["fx"]
        fx_keys = list(fx)

        for char in _visible_cast(beat["cast"]):
            if char.get("通讯框") is not None and char.get("name"):
                remote_chars.add(char["name"].lower())
        if fx.get("通讯框") is not None:
            remote_chars.update(c["name"].lower() for c in _visible_cast(beat["cast"]))

        # --- state ops from effects ---
        if fx.get("bin", "").isdigit():
            ev = {"t": "bg", "id": f"bg_{fx['bin']}"}
            # black_on right before a bg switch means fade-in-from-black
            if fx.get("睁眼") is not None:
                # Keep the new background covered until the following
                # eyes_open effect reveals it; do not auto-fade it first.
                ev["transition"] = "eyes_open"
            elif any(fx.get(k) is not None for k in ("黑屏2", "黑点2")):
                ev["transition"] = "fade_black"
            elif fx.get("bin_slowin") is not None:
                ev["transition"] = "slow_fade"
            if fx.get("cgdelay") not in (None, ""):
                try:
                    ev["delay_ms"] = max(0, int(float(fx["cgdelay"].replace(",", ".")) * 1000))
                except ValueError:
                    pass
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
                # In the original scripts tag order matters.  A transient
                # black fade followed by a persistent black tag means the new
                # BIN is prepared behind the blackout; animating the earlier
                # fade leaks that image for a few frames before the dialogue.
                if tag in ("黑屏2", "黑点2"):
                    here = fx_keys.index(tag)
                    if any(p in fx_keys[here + 1:] for p in ("黑屏1", "黑点1")):
                        continue
                events.append({"t": "effect", "kind": _EFFECT_MAP[tag]})
        # gfStory-en renders every <CG>a,b,c</CG> entry as a background step
        # followed by progressively longer ellipsis text.
        for i, cg in enumerate((p.strip() for p in fx.get("cg", "").split(","))):
            if cg.isdigit():
                events.append({"t": "bg", "id": f"bg_{cg}", "cg": True})
                events.append({"t": "say", "name": "", "text": ["……" * (i + 1)],
                               "chars": []})

        # --- stage updates ---
        events += _stage_diff(prev_cast, beat["cast"])
        prev_cast = beat["cast"]

        # --- dialogue / choices ---
        if beat.get("choice"):
            events.append({
                "t": "choice",
                "kind": beat["choice"]["kind"],
                "options": [o.strip() for o in beat["choice"]["options"]],
                "stage": _stage_snapshot(beat["cast"], remote_chars),
            })
        elif beat["text"]:
            say = {
                "t": "say",
                "name": _speaker(beat),
                "text": beat["text"],
                "chars": [[slug(c["name"]), c["expr"]] for c in _visible_cast(beat["cast"])],
                "stage": _stage_snapshot(beat["cast"], remote_chars),
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
