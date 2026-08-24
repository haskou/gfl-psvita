#!/usr/bin/env python3
"""Acceptance audit for every imported story and every resolvable asset."""
import json
import pathlib
import struct
from unittest import SkipTest

ROOT = pathlib.Path(__file__).parent


def norm(value):
    return "".join(c for c in value.lower() if c.isalnum())


def test_manifest_complete():
    if not (ROOT / "assets/manifest.json").is_file():
        raise SkipTest("requires a complete local assets import")
    manifest = json.loads((ROOT / "assets/manifest.json").read_text())
    scene_paths = sorted((ROOT / "assets/scenes").glob("*.ir.json"))
    scene_ids = {p.name.removesuffix(".ir.json") for p in scene_paths}
    assert set(manifest["scenes"]) == scene_ids
    source_index = json.loads((ROOT / "assets/source_index.json").read_text())
    assert scene_ids == set(source_index)
    assert len(scene_ids) == 3582, f"expected the complete 3582-scene corpus, got {len(scene_ids)}"

    resolved = {**manifest["img"], **manifest["aud"]}

    required_runtime_assets = {
        "ui_call_frame", "ui_gf_system", "ui_loaded_circle",
        "se_AVG_tele_connect", "se_AVG_tele_disconnect",
    }
    required_runtime_assets |= {"event_operation_cube_logo", "event_operation_cube_poster"}
    assert required_runtime_assets <= resolved.keys(), (
        f"runtime call UI/audio missing from manifest: "
        f"{sorted(required_runtime_assets - resolved.keys())}"
    )
    for key, rel in resolved.items():
        path = ROOT / rel
        assert path.is_file() and path.stat().st_size > 0, f"{key} -> {path} missing/empty"
        if key.startswith("spr_"):
            header = path.read_bytes()[:26]
            assert header[:8] == b"\x89PNG\r\n\x1a\n", f"{key} is not a PNG"
            width, height = struct.unpack(">II", header[16:24])
            color_type = header[25]
            assert height <= 544, f"{key} is {width}x{height}, larger than Vita native height"
            assert color_type == 6, f"{key} is not true-colour RGBA (PNG type {color_type})"

    resdata = json.loads((ROOT / "assets/bundles/us_resdata.json").read_text())
    prefab_names = set()
    for bundle in resdata["BaseAssetBundles"] + resdata["AddAssetBundles"]:
        if bundle["assetBundleName"].startswith("avgpicprefabs"):
            for resource in bundle["assetAllRes"]:
                prefab_names.add(norm(resource["pathKey"].rsplit("/", 1)[-1].rsplit(".", 1)[0]))
    aliases = {
        "npc-helian": "npchelian", "npc-kyruger": "npckyruger",
        "stu20ar-15": "ar15", "m4u20sopmodu20": "m4sopmodii", "ro": "ro635",
    }

    checked = set()
    commands = {"bgm_BGM_Pause", "bgm_BGM_PAUSE", "bgm_BGM_UnPause", "bgm_BGM_UNPAUSE",
                "se_Stop_AVG_loop", "se_stop_AVG_applause_indoor"}
    legacy_missing = {
        "bgm_GF_xBH2_Menu", "bgm_SpecialActivity", "bgm_m_va_spirit_potion",
        "se_AVG_Division_Flare_Gun", "se_AVG_Division_Camera",
        "bgm_BGM_SpecialOPS", "bgm_Companion-34", "bg_252", "bg_253", "bg_562",
    }
    missing_media = []
    missing_real_sprites = []
    text_only = set()
    for path in scene_paths:
        scene = json.loads(path.read_text())
        for ev in scene["events"]:
            if ev["t"] in ("bg", "music", "sfx"):
                if ev["id"] not in resolved and ev["id"] not in commands | legacy_missing:
                    missing_media.append((scene["id"], ev["id"]))
                checked.add(ev["id"])
            if ev["t"] == "say":
                for sprite in ev.get("stage", []):
                    key = f"spr_{sprite['char']}_{sprite['expr']}"
                    if key in resolved:
                        checked.add(key)
                        continue
                    candidate = aliases.get(sprite["char"], norm(sprite["char"]))
                    if candidate in prefab_names:
                        missing_real_sprites.append((scene["id"], key))
                    else:
                        text_only.add(sprite["char"])

    assert not missing_media, f"unresolved media references: {missing_media[:30]}"
    assert not missing_real_sprites, f"resolvable sprites missing: {missing_real_sprites[:30]}"
    print(f"  ({len(scene_ids)} scenes, {len(checked)} referenced asset keys verified; "
          f"{len(text_only)} narrator/voice labels intentionally text-only)")


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
