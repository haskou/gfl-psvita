#!/usr/bin/env python3
"""Step 02b extractor: bundles in assets/bundles/ -> game assets in assets/.

Runs INSIDE docker (gflvn-tools image: UnityPy + magick + vgmstream + ffmpeg).

  docker run --rm -v "$PWD:/work" -w /work gflvn-tools python3 tools/extract_assets.py -1-1-1

Outputs:
  assets/img/bg_<bin>.png          backgrounds for the scene's <BIN> refs
  assets/img/spr_<char>_<expr>.png dialogue sprites
  assets/aud/*.ogg                 BGM/SE
  assets/manifest.json             IR asset key -> file map + unresolved warnings
"""
import json
import gc
import subprocess
import sys
import zipfile
from pathlib import Path

import UnityPy

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent.parent / "src"))
from gflvn.ir import slug  # same ASCII key derivation as the exporter
BUNDLES = Path("assets/bundles")
IMG = Path("assets/img")
AUD = Path("assets/aud")
FONTS = Path("runtime/fonts")
SPRITE_MAX_HEIGHT = 544  # PS Vita's native framebuffer height

# prefab bundle -> texture bundles that hold its pics (resolved empirically)
PREFAB_SOURCES = {
    "avgpicprefabs_gunum": ["character_06typesmg"],
    "avgpicprefabs_gunhk": ["character_hk416"],
    "avgpicprefabs_npche": ["character_npc_helianthus"],
    "avgpicprefabs_npcky": ["character_npc_kryuger"],
}
# narrator name (slugged, as it appears in scene events) -> prefab name
NAME_OVERRIDES = {
    slug("赫丽安"): "npc-helian",
    slug("克鲁格"): "npc-kyruger",
    slug("ST AR-15"): "ar15",
    slug("M4 SOPMOD "): "m4 sopmod ii",
    "m4u20sopmodu20ii": "m4 sopmod ii",
    "m4u20sopmodu20iimod": "m4 sopmod iimod",
    "m4u20sopmodu20iimodnoarmor": "m4 sopmod iimod-noarmor",
    "m4u20sopmodu20iitarot": "m4 sopmod iitarot",
    "ro": "ro635",
}

AUDIO_ALIASES = {
    "Room": "BGM_Room",
    "bossbattle_loop": "BGM_Boss",
    "GF_Daily": "GF_Daily_01_loop",
    # The current EN scripts contain several stale/translated cue names.  The
    # audio banks themselves use the names on the right (as confirmed by the
    # decoded ACB cue table), so keep the correction at import time rather
    # than teaching the player about individual story typos.
    "GF_Café": "GF_Cafe",
    "GF_café": "GF_Cafe",
    "Gunfire": "AVG_Gunfire",
    "AVG_The_Division_Grenade_Explosion": "AVG_The_Division_Grenade",
    "AVG_20Winter_Error": "AVG_21Winter_Error",
    "AVG_20Winter_Open_Light": "AVG_21Winter_Open_Light",
    "AVG_Gray_Hand_Mech": "AVG_Grey_Hand_Mech",
    "AVG_2023Slow_Heart_telescreen3": "AVG_2023Slow_Heart_Monitor3",
}
AUDIO_COMMANDS = {"BGM_Pause", "BGM_PAUSE", "BGM_UnPause", "BGM_UNPAUSE",
                  "Stop_AVG_loop", "stop_AVG_applause_indoor"}


def prefab_key(name):
    """Unity display names vary only by spaces/dashes in many scripts."""
    return "".join(c for c in name.lower() if c.isalnum())


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def load_env(names):
    env = UnityPy.Environment()
    for n in names:
        p = Path(n) if str(n).endswith(".ab") else BUNDLES / f"{n}.ab"
        if p.exists():
            env.load_file(str(p))
    return env


def tex_index(env):
    idx = {}
    for obj in env.objects:
        if obj.type.name in ("Texture2D", "Sprite"):
            idx.setdefault(obj.path_id, []).append(obj)
    return idx


def obj_image(obj):
    return obj.read().image


def save_png(image, dest, quant=True):
    dest.parent.mkdir(parents=True, exist_ok=True)
    image.save(dest)
    if quant:
        r = subprocess.run(["pngquant", "--force", "--ext", ".png", "--skip-if-larger", str(dest)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode not in (0, 98):  # 98 = skipped, larger result
            r.check_returncode()


def finish_sprite(source, dest):
    """Downsample once from the lossless source to the Vita's display height.

    Keep sprites as true-colour RGBA. Quantising the 1024/2048 px originals to
    256 colours caused visible gradients/edges, while retaining those oversized
    textures wasted both decode time and GPU memory on a 960x544 display.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    run([
        "magick", str(source),
        "-filter", "Lanczos",
        "-resize", f"x{SPRITE_MAX_HEIGHT}>",
        "-strip",
        "-define", "png:color-type=6",
        str(dest),
    ])


def save_sprite(image, dest):
    tmp = dest.with_name(f".{dest.name}.source.png")
    image.save(tmp)
    try:
        finish_sprite(tmp, dest)
    finally:
        tmp.unlink(missing_ok=True)


def merge_alpha(pic_obj, alpha_obj, dest):
    """Sprite + separate alpha-mask texture -> transparent PNG (gfunpack method)."""
    tmp = dest.parent
    tmp.mkdir(parents=True, exist_ok=True)
    a, b, c, merged = (tmp / "_p.png", tmp / "_a.png", tmp / "_d.png",
                       tmp / "_merged.png")
    obj_image(pic_obj).save(a)
    ai = obj_image(alpha_obj)
    if ai.mode != "RGBA" or ai.getextrema()[3][0] == 255:
        # alpha texture is a mask in RGB, resize to sprite dims then copy channel
        ai.save(b)
        run(["magick", str(a), "-set", "option:dims", "%wx%h", str(b),
             "-delete", "0", "-resize", "%[dims]", str(c)])
        run(["magick", str(a), str(c), "-compose", "copy-opacity", "-composite",
             str(merged)])
    else:
        ai.save(merged)
        # Some bundles store the already-composited sprite in picAlpha. If it
        # unexpectedly has no useful transparency, fall back to the colour pic.
        if ai.getextrema()[3][0] == 255:
            obj_image(pic_obj).save(merged)
    finish_sprite(merged, dest)
    for f in (a, b, c, merged):
        f.unlink(missing_ok=True)


def extract_bgs(profiles, bins):
    out = {f"bg_{b}": str(IMG / f"bg_{b}.png")
           for b in bins if (IMG / f"bg_{b}.png").exists()}
    missing = [b for b in bins if f"bg_{b}" not in out]
    if not missing:
        return out
    env = load_env([n for n in BUNDLES.glob("resource_avgtexture*.ab")])
    by_name = {}
    for path, ptr in env.container.items():
        if "/avgtexture/" in path.lower():
            by_name[path.rsplit("/", 1)[-1].rsplit(".", 1)[0].lower()] = ptr
    for bin_idx in missing:
        if not str(bin_idx).isdigit():
            print(f"WARN invalid BIN '{bin_idx}'", file=sys.stderr)
            continue
        profile_idx = int(bin_idx)
        if profile_idx < 0 or profile_idx >= len(profiles):
            print(f"WARN BIN {bin_idx} outside profiles table", file=sys.stderr)
            continue
        name = profiles[profile_idx]
        ptr = by_name.get(name.lower())
        if ptr is None:
            print(f"WARN bg '{name}' (BIN {bin_idx}) not found", file=sys.stderr)
            continue
        dest = IMG / f"bg_{bin_idx}.png"
        if not dest.exists():
            save_png(ptr.read().image, dest)
        out[f"bg_{bin_idx}"] = str(dest)
    return out


def extract_game_font():
    """Export the game's own Latin+CJK font so untranslated lines render on Vita."""
    dest = FONTS / "NotoSansHans-Regular.ttf"
    if dest.exists():
        return
    env = load_env(["asset_fonts"])
    for obj in env.objects:
        if obj.type.name != "Font":
            continue
        font = obj.read()
        if (getattr(font, "m_Name", "") or "").upper() == "NOTOSANSHANS-REGULAR":
            data = font.m_FontData
            data = data.tobytes() if hasattr(data, "tobytes") else bytes(data)
            FONTS.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            return
    raise RuntimeError("NOTOSANSHANS-REGULAR missing from asset_fonts")


def prefab_dependencies(bundle_names):
    """Read each bundle's AssetBundle m_Dependencies -> set of dependency names."""
    deps = set()
    for n in bundle_names:
        env = UnityPy.Environment()
        env.load_file(str(BUNDLES / f"{n}.ab"))
        for o in env.objects:
            if o.type.name == "AssetBundle":
                deps.update(o.read_typetree().get("m_Dependencies", []))
    return deps


def extract_sprites(cast, force=False):
    """cast: [(charname_lower, expr)] -> {key: path, ...}, warnings."""
    out = {}
    resolved = set()
    for name, expr in cast:
        dest = IMG / f"spr_{slug(name)}_{expr}.png"
        if dest.exists() and not force:
            out[f"spr_{slug(name)}_{expr}"] = str(dest)
            resolved.add((name, expr))
    wanted = {}
    for name, expr in cast:
        if (name, expr) not in resolved:
            wanted.setdefault(prefab_key(NAME_OVERRIDES.get(name, name)), []).append((name, expr))
    if not wanted:
        return out
    have = sorted(p.stem for p in BUNDLES.glob("avgpicprefabs_*.ab"))
    # A full-story run references several GB of character bundles. Loading all
    # of them into one UnityPy Environment exceeds normal desktop/container RAM;
    # each prefab plus its dependencies is self-contained, so process one at a time.
    for bundle in have:
        deps = sorted(prefab_dependencies([bundle]))
        env = load_env([bundle] + deps)
        idx = tex_index(env)
        go_to_prefab = {}
        for path, ptr in env.container.items():
            if "/avgpicprefabs/" in path.lower() and path.endswith(".prefab"):
                go_to_prefab[ptr.m_PathID] = prefab_key(path.rsplit("/", 1)[-1][:-7])
        holders = {}
        for obj in env.objects:
            if obj.type.name != "MonoBehaviour":
                continue
            tt = obj.read_typetree()
            if "pic" not in tt:
                continue
            pname = go_to_prefab.get(tt["m_GameObject"]["m_PathID"])
            if pname not in wanted:
                continue
            holders.setdefault(pname, []).extend(
                (i, p["m_PathID"], a["m_PathID"])
                for i, (p, a) in enumerate(zip(tt["pic"], tt["picAlpha"]))
            )
        for pname, requests in wanted.items():
            for name, expr in requests:
                if (name, expr) in resolved:
                    continue
                entry = next((e for e in holders.get(pname, []) if e[0] == expr), None)
                if entry is None:
                    continue
                _, pid, aid = entry
                pics, alphas = idx.get(pid, []), idx.get(aid, [])
                if not pics:
                    continue
                dest = IMG / f"spr_{slug(name)}_{expr}.png"
                if force or not dest.exists():
                    if alphas and alphas[0].path_id != pics[0].path_id:
                        merge_alpha(pics[0], alphas[0], dest)
                    else:
                        save_sprite(obj_image(pics[0]), dest)
                out[f"spr_{slug(name)}_{expr}"] = str(dest)
                resolved.add((name, expr))
        del idx, env
        gc.collect()
        if len(resolved) == len(cast):
            break
    for pname, requests in wanted.items():
        for name, expr in requests:
            if (name, expr) not in resolved:
                print(f"WARN sprite {pname}[{expr}] unresolved", file=sys.stderr)
    return out


def audio_cues(wanted_bgm, wanted_se):
    """Extract ACB banks -> ogg; return {cue_name_lower: path}."""
    out = {p.stem.lower(): str(p) for p in AUD.glob("*.ogg")}
    for dat in sorted(BUNDLES.glob("*.acb.dat")):
        work = AUD / "_work" / dat.stem
        work.mkdir(parents=True, exist_ok=True)
        done = work / ".decoded"
        if done.exists():
            continue
        try:
            with zipfile.ZipFile(dat) as z:
                z.extractall(work)
        except zipfile.BadZipFile:
            continue
        acb = next(iter(work.glob("*.acb.bytes")), None) or next(iter(work.glob("*.acb")), None)
        if acb is None:
            continue
        if acb.suffix == ".bytes":
            acb.rename(acb.with_suffix(""))
            acb = acb.with_suffix("")
        run(["vgmstream-cli", str(acb), "-o", str(work / "?n.wav"), "-S", "0"])
        acb.unlink(missing_ok=True)
        done.touch()
    for wav in AUD.rglob("_work/*/*.wav"):
        key = wav.stem.lower()
        dest = AUD / f"{wav.stem}.ogg"
        if not dest.exists():
            run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), str(dest)])
        out[key] = str(dest)
        wav.unlink()
    return out


def ensure_bundles(names):
    """Download missing bundles by assetBundleName using the local resdata json."""
    import urllib.request
    rd_path = BUNDLES / "us_resdata.json"
    if not names or not rd_path.exists():
        return
    rd = json.loads(rd_path.read_text())
    by_name = {b["assetBundleName"]: b
               for b in rd["BaseAssetBundles"] + rd["AddAssetBundles"]}
    for n in sorted(set(names)):
        dest = BUNDLES / f"{n}.ab"
        if n in by_name and not dest.exists():
            url = rd["resUrl"] + by_name[n]["resname"] + ".ab"
            print(f"auto-download bundle: {n}")
            urllib.request.urlretrieve(url, dest)


def find_dat_entry(cue):
    import urllib.request
    rd = json.loads((BUNDLES / "us_resdata.json").read_text())
    for b in rd["bytesData"]:
        if b["fileName"].lower() == cue.lower():
            dest = BUNDLES / (b["fileName"] + ".dat")
            if not dest.exists():
                print(f"auto-download audio bank: {b['fileName']}")
                urllib.request.urlretrieve(rd["resUrl"] + b["resname"] + ".dat", dest)
            return True
    return False


def main():
    extract_game_font()
    scenes_dir = Path("assets/scenes")
    args = sys.argv[1:]
    rebuild_sprites = "--rebuild-sprites" in args
    args = [arg for arg in args if arg != "--rebuild-sprites"]
    scene_ids = args or sorted(p.name.replace(".beats.json", "") for p in scenes_dir.glob("*.beats.json"))
    docs = {}
    for sid in scene_ids:
        docs[sid] = json.loads((scenes_dir / f"{sid}.beats.json").read_text())
    manifest = {"scenes": scene_ids, "img": {}, "aud": {}, "warnings": []}

    profiles = [
        ln.strip()
        for ln in (Path("research/gf-data-us/asset/avgtxt/profiles.txt")
                   if Path("research/gf-data-us/asset/avgtxt/profiles.txt").exists()
                   else Path("research/GirlsFrontlineData/en-US/asset_textes/avgtxt/profiles.txt"))
                  .read_text(encoding="utf-8").splitlines()
        if ln.strip()
    ]

    # what do these scenes need? (merged)
    bins, sprites, bgms, ses = [], [], [], []
    for doc in docs.values():
        for beat in doc["beats"]:
            fx = beat["fx"]
            if fx.get("bin", "").isdigit() and fx["bin"] not in bins:
                bins.append(fx["bin"])
            for cg in (part.strip() for part in fx.get("cg", "").split(",")):
                if cg.isdigit() and cg not in bins:
                    bins.append(cg)
            if fx.get("bgm") and fx["bgm"] not in bgms:
                bgms.append(fx["bgm"])
            for k in ("se", "se1", "se2", "se3"):
                if fx.get(k) and fx[k] not in ses:
                    ses.append(fx[k])
            for c in beat["cast"]:
                if (c["name"] and c.get("expr") is not None
                        and (slug(c["name"]), c["expr"]) not in sprites):
                    sprites.append((slug(c["name"]), c["expr"]))

    # make sure every dependency of our prefab bundles is present before extraction
    have_prefabs = [p.stem for p in BUNDLES.glob("avgpicprefabs_*.ab")]
    ensure_bundles(prefab_dependencies(have_prefabs))

    manifest["img"].update(extract_bgs(profiles, bins))
    manifest["img"].update(extract_sprites(sprites, force=rebuild_sprites))

    # audio: script name -> cue name comes from audiotemplate.txt
    tmpl = {}
    tenv = load_env(["asset_textes"])
    for obj in tenv.objects:
        if obj.type.name == "TextAsset":
            d = obj.read()
            nm = getattr(d, "m_Name", "") or ""
            if "audiotemplate" in nm.lower():
                data = d.m_Script
                data = data.encode() if isinstance(data, str) else (
                    data.tobytes() if hasattr(data, "tobytes") else bytes(data))
                for line in data.decode(errors="ignore").split("\n"):
                    line = line.split("//")[0].strip()
                    f = line.split("|")
                    if len(f) >= 3:
                        tmpl[f[1]] = f[2]

    cues = audio_cues(bgms, ses)

    # Fetch every missing cue bank first, then decode once. The previous
    # per-cue retry re-decoded all earlier banks up to hundreds of times.
    fetched = False
    for name in bgms + ses:
        cue = tmpl.get(name, name).lower()
        if cue not in cues:
            fetched = find_dat_entry(cue + ".acb") or fetched
    if fetched:
        cues.update(audio_cues(bgms, ses))

    def resolve_audio(name):
        cue = tmpl.get(name, AUDIO_ALIASES.get(name, name)).lower()
        if cue in cues:
            return ("bgm_" if name in bgms else "se_") + name, cues[cue]
        return None, None
    for name in bgms + ses:
        if name in AUDIO_COMMANDS:
            continue
        key, path = resolve_audio(name)
        if path:
            manifest["aud"][key] = path
        else:
            manifest["warnings"].append(f"audio '{name}' (cue '{tmpl.get(name, name)}') unresolved")

    # Runtime-owned UI and call-session cues are not referenced explicitly by
    # the AVG scripts, so keep them in the manifest on every full regeneration.
    manifest["img"].update({
        "ui_call_frame": "assets/img/ui_call_frame.png",
        "ui_gf_system": "assets/img/ui_gf_system.png",
        "ui_loaded_circle": "assets/img/ui_loaded_circle.png",
    })
    for event_art in sorted(IMG.glob("event_*")):
        manifest["img"][event_art.stem] = event_art.as_posix()
    manifest["aud"].update({
        "se_AVG_tele_connect": "assets/aud/AVG_tele_connect.ogg",
        "se_AVG_tele_disconnect": "assets/aud/AVG_tele_disconnect.ogg",
    })

    for k in sorted(manifest["img"]):
        print(k, "->", manifest["img"][k])
    for k in sorted(manifest["aud"]):
        print(k, "->", manifest["aud"][k])
    for w in manifest["warnings"]:
        print("WARN", w)
    Path(f"assets/manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
