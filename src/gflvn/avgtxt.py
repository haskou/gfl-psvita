#!/usr/bin/env python3
"""GFL avgtxt story script -> typed beats.

Grammar (docs/investigation.md section 1.2):
    <narrators>||<effects>:<content>
One line = one beat. Unknown tags degrade to `beat["fx"]` entries, never errors.

Usage: python3 avgtxt.py SCRIPT.txt [-o out.json] [--update-golden]
"""
import json
import re
import sys

SPEAKER_RE = re.compile(r"<speaker>(.*)</speaker>", re.IGNORECASE)
SPRITE_RE = re.compile(r"^([^()<>]*)\((\d*)\)")
TAG_NAME_RE = re.compile(r"</?([^<>]+)(?:=[^<>]*)?>")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f]")

# Tags promoted to explicit beat fields; everything else lands verbatim in fx.
VALUED_TAGS = {"bin", "bgm", "se", "se1", "se2", "se3", "cg", "分支", "边框"}
FLAG_TAGS = {
    "night", "通讯框", "回忆", "关闭蒙版", "睁眼",
    "黑屏1", "黑屏2", "黑点1", "黑点2", "白屏1", "白屏2", "闪屏",
    "震屏", "震屏3", "shake", "grey", "拉伸", "平移", "立绘振动",
    "火花", "下雪", "火焰", "火焰1", "火焰2", "火焰3",
}
KNOWN_CONTENT_TAGS = {"c", "r", "t", "cg", "va11", "color", "size"}

CHOICE_MARKERS = ("<cg>", "<c>", "<r>", "<t>")  # order matters: most specific first


def _parse_effects(effects: str):
    """'<BIN>3</BIN><Night>' -> ({'bin': '3', 'night': ''}, ['bin', 'night'])."""
    values = {}
    for name in TAG_NAME_RE.findall(effects):
        full = f"<{name}>"
        close = f"</{name}>"
        if close in effects and full in effects:
            start = effects.index(full)
            end = effects.index(close)
            value = effects[start + len(full):end]
            # A few upstream lines contain `<SE2><SE2>Gunfight</SE2></SE2>`.
            # Treat the duplicated wrapper as the same single valued tag.
            value = re.sub(rf"</?{re.escape(name)}>", "", value, flags=re.IGNORECASE)
            values[name] = value
        else:
            values.setdefault(name, "")
    return values


def _parse_narrators(narrators: str, warnings: list):
    """'HK416(2)<Speaker>416</Speaker>;UMP45(0)' -> (cast, display_speaker)."""
    cast = []
    display = ""
    for narrator in narrators.split(";"):
        m = SPEAKER_RE.search(narrator)
        if m:
            display = m.group(1)
            narrator = SPEAKER_RE.sub("", narrator)
        m = SPRITE_RE.match(narrator)
        if not m:
            warnings.append(f"unrecognized narrator `{narrator}`")
            continue
        name, expr = m.group(1), m.group(2)
        attrs = {tag.lower(): value for tag, value in _parse_effects(narrator).items()
                 if tag.lower() != "speaker"}
        if "#" in name:  # sprite modifier e.g. Name#隐身
            name, mod = name.split("#", 1)
            cast.append({"name": name, "expr": int(expr or 0), "mod": mod, **attrs})
        else:
            cast.append({"name": name, "expr": int(expr) if expr else None, **attrs})
    return cast, display


def _split_choices(content: str):
    """Return (dialogue_text, choice_kind, [option_texts])."""
    for marker in CHOICE_MARKERS:
        if marker in content:
            parts = content.split(marker)
            kind = marker.strip("<>")
            return parts[0], kind, [p for p in parts[1:] if p.strip()]
    return content, "", []


def _extract_content_effects(content: str, fx: dict):
    """Some later scripts append scene effects after the spoken text."""
    for tag in TAG_NAME_RE.findall(content):
        key = tag.lower()
        if tag in FLAG_TAGS or key in FLAG_TAGS:
            fx[key] = ""
            content = re.sub(rf"</?{re.escape(tag)}(?:=[^<>]*)?>", "", content,
                             flags=re.IGNORECASE)
    return content


def parse_line(line: str, warnings: list):
    line = CONTROL_RE.sub(" ", line.strip())
    if not line:
        return None
    line = line.replace("：", ": ")  # full-width colon typo in some scripts
    if ":" not in line:
        warnings.append(f"no colon, skipped: `{line[:60]}`")
        return None
    meta, content = line.split(":", 1)
    if "||" not in meta:
        warnings.append(f"no || separator, skipped: `{line[:60]}`")
        return None
    narrators, effect_str = meta.split("||", 1)

    fx = {}
    for tag, value in _parse_effects(effect_str).items():
        key = tag.lower()
        if key in VALUED_TAGS or tag in VALUED_TAGS:
            fx[key] = value
        elif tag in FLAG_TAGS or key in FLAG_TAGS:
            fx[key] = ""
        elif key not in KNOWN_CONTENT_TAGS and key != "speaker":
            fx[key] = value  # unknown: preserve verbatim, renderer decides
            warnings.append(f"unknown tag `<{tag}>`")

    cast, display = _parse_narrators(narrators, warnings)

    beat = {
        "speaker": display,
        "cast": cast,
        "text": [],
        "fx": fx,
    }

    content = _extract_content_effects(content, fx)
    dialogue, choice_kind, options = _split_choices(content)
    beat["text"] = [p.strip() for p in dialogue.split("+") if p.strip()]
    if choice_kind:
        beat["choice"] = {"kind": choice_kind, "options": options}

    return beat


def parse_script(text: str, filename: str = "<script>"):
    """Whole script -> {"id", "beats", "warnings"}."""
    warnings = []
    beats = []
    for i, line in enumerate(text.split("\n"), 1):
        try:
            beat = parse_line(line, warnings)
        except Exception as exc:  # never fatal: log and move on
            warnings.append(f"line {i}: {type(exc).__name__}: {exc}")
            continue
        if beat:
            beat["line"] = i
            beats.append(beat)
    return {"id": filename, "beats": beats, "warnings": warnings}


def main():
    args = sys.argv[1:]
    update_golden = "--update-golden" in args
    out_path = None
    if "-o" in args:
        i = args.index("-o")
        out_path = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if not a.startswith("--")]
    src = args[0]

    doc = parse_script(open(src, encoding="utf-8").read(), src.rsplit("/", 1)[-1])
    js = json.dumps(doc, ensure_ascii=False, indent=1)

    if out_path:
        open(out_path, "w", encoding="utf-8").write(js + "\n")
    else:
        print(js)
    for w in doc["warnings"]:
        print(f"warning: {w}", file=sys.stderr)
    if update_golden:
        print("golden updated")


if __name__ == "__main__":
    main()
