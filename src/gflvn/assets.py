"""Step 02 asset resolver v0: beat asset references -> opaque keys + warnings.

Works offline against the text mirror (profiles.txt). Actual PNG/OGG extraction
from Unity bundles is a later importer step (needs the archived bundle set).
"""
from pathlib import Path


def load_profiles(path) -> list[str]:
    """profiles.txt -> ordered background/CG names. BIN index = 0-based line."""
    return [ln.strip() for ln in Path(path).read_text(encoding="utf-8").splitlines() if ln.strip()]


def resolve_scene(doc: dict, profiles: list[str]) -> dict:
    """Parsed script doc -> {"bg", "bgm", "se", "sprites", "warnings"}.

    bg: BIN index -> profile name; bgm/se: unique cue names in first-seen order;
    sprites: unique [lower(name), expr] pairs in first-seen order.
    """
    out = {"bg": {}, "bgm": {}, "se": {}, "sprites": {}, "warnings": []}
    for beat in doc["beats"]:
        fx = beat["fx"]
        if "bin" in fx:
            try:
                i = int(fx["bin"])
            except (TypeError, ValueError):
                out["warnings"].append(f"invalid BIN {fx['bin']!r}")
                i = -1
            if 0 <= i < len(profiles):
                out["bg"].setdefault(fx["bin"], profiles[i])
            elif i >= 0:
                out["warnings"].append(f"BIN {i} out of range (0..{len(profiles)-1})")
        for cg in (part.strip() for part in fx.get("cg", "").split(",")):
            if not cg:
                continue
            try:
                i = int(cg)
            except ValueError:
                out["warnings"].append(f"invalid CG {cg!r}")
                continue
            if 0 <= i < len(profiles):
                out["bg"].setdefault(cg, profiles[i])
            else:
                out["warnings"].append(f"CG {i} out of range (0..{len(profiles)-1})")
        for key, bucket in (("bgm", "bgm"), ("se", "se"), ("se1", "se"), ("se2", "se"), ("se3", "se")):
            if fx.get(key):
                out[bucket].setdefault(fx[key], None)
        for ch in beat["cast"]:
            if ch["name"] and ch.get("expr") is not None:
                out["sprites"].setdefault((ch["name"].lower(), ch["expr"]), None)
    out["sprites"] = [list(k) for k in out["sprites"]]
    return out


def scene_asset_report(doc: dict, profiles_path) -> dict:
    return resolve_scene(doc, load_profiles(profiles_path))
