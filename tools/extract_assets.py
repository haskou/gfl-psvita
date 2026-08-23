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
}


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


def merge_alpha(pic_obj, alpha_obj, dest):
    """Sprite + separate alpha-mask texture -> transparent PNG (gfunpack method)."""
    tmp = dest.parent
    tmp.mkdir(parents=True, exist_ok=True)
    a, b, c = tmp / "_p.png", tmp / "_a.png", tmp / "_d.png"
    obj_image(pic_obj).save(a)
    ai = obj_image(alpha_obj)
    if ai.mode != "RGBA" or ai.getextrema()[3][0] == 255:
        # alpha texture is a mask in RGB, resize to sprite dims then copy channel
        ai.save(b)
        run(["magick", str(a), "-resize", f"{ai.width}x{ai.height}!", str(c)])
        run(["magick", str(a), str(c), "-compose", "copy-opacity", "-composite", str(dest)])
    else:
        ai.save(dest)
    for f in (a, b, c):
        f.unlink(missing_ok=True)
    run(["pngquant", "--force", "--ext", ".png", "--skip-if-larger", str(dest)])


def extract_bgs(profiles, bins):
    env = load_env([n for n in BUNDLES.glob("resource_avgtexture*.ab")])
    by_name = {}
    for path, ptr in env.container.items():
        if "/avgtexture/" in path.lower():
            by_name[path.rsplit("/", 1)[-1].rsplit(".", 1)[0].lower()] = ptr
    out = {}
    for bin_idx in bins:
        name = profiles[int(bin_idx)]
        ptr = by_name.get(name.lower())
        if ptr is None:
            print(f"WARN bg '{name}' (BIN {bin_idx}) not found", file=sys.stderr)
            continue
        dest = IMG / f"bg_{bin_idx}.png"
        save_png(ptr.read().image, dest)
        out[f"bg_{bin_idx}"] = str(dest)
    return out


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


def extract_sprites(cast):
    """cast: [(charname_lower, expr)] -> {key: path, ...}, warnings."""
    prefabs_needed = sorted({NAME_OVERRIDES.get(n, n) for n, _ in cast})
    # figure out which prefab bundles we actually have and need
    have = sorted(p.stem for p in BUNDLES.glob("avgpicprefabs_*.ab"))
    env = load_env(have + sorted(prefab_dependencies(have)))
    idx = tex_index(env)

    # container path -> prefab display name ('.../UMP45.prefab' -> 'UMP45')
    go_to_prefab = {}
    for path, ptr in env.container.items():
        if "/avgpicprefabs/" in path.lower() and path.endswith(".prefab"):
            go_to_prefab[ptr.m_PathID] = path.rsplit("/", 1)[-1][:-7]

    # collect DialoguePicHolder MonoBehaviour per prefab
    holders = {}  # prefab_name -> [(pic_path_id, alpha_path_id)]
    for obj in env.objects:
        if obj.type.name != "MonoBehaviour":
            continue
        tt = obj.read_typetree()
        if "pic" not in tt:
            continue
        pname = go_to_prefab.get(tt["m_GameObject"]["m_PathID"])
        if not pname:
            continue
        entries = list(zip(tt["pic"], tt["picAlpha"]))
        holders.setdefault(pname.lower(), []).extend(
            [(i, p["m_PathID"], a["m_PathID"]) for i, (p, a) in enumerate(entries)]
        )

    out = {}
    for name, expr in cast:
        pname = NAME_OVERRIDES.get(name, name).lower()
        entries = [e for e in holders.get(pname, []) if e[0] == expr]
        if not entries:
            print(f"WARN sprite {pname}[{expr}] has no prefab entry", file=sys.stderr)
            continue
        _, pid, aid = entries[0]
        pics = idx.get(pid, [])
        alphas = idx.get(aid, [])
        if not pics:
            print(f"WARN sprite {pname}[{expr}] pic path_id {pid} missing", file=sys.stderr)
            continue
        dest = IMG / f"spr_{slug(name)}_{expr}.png"
        if alphas and alphas[0].path_id != pics[0].path_id:
            merge_alpha(pics[0], alphas[0], dest)
        else:
            save_png(obj_image(pics[0]), dest)
        out[f"spr_{slug(name)}_{expr}"] = str(dest)
    return out


def audio_cues(wanted_bgm, wanted_se):
    """Extract ACB banks -> ogg; return {cue_name_lower: path}."""
    out = {}
    for dat in sorted(BUNDLES.glob("*.acb.dat")):
        work = AUD / "_work" / dat.stem
        work.mkdir(parents=True, exist_ok=True)
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
    scenes_dir = Path("assets/scenes")
    scene_ids = sys.argv[1:] or sorted(p.name.replace(".beats.json", "") for p in scenes_dir.glob("*.beats.json"))
    docs = {}
    for sid in scene_ids:
        docs[sid] = json.loads((scenes_dir / f"{sid}.beats.json").read_text())
    manifest = {"scenes": scene_ids, "img": {}, "aud": {}, "warnings": []}

    profiles = [
        ln.strip()
        for ln in (Path("research/GirlsFrontlineData/en-US/asset_textes/avgtxt/profiles.txt")).read_text(encoding="utf-8").splitlines()
        if ln.strip()
    ]

    # what do these scenes need? (merged)
    bins, sprites, bgms, ses = [], [], [], []
    for doc in docs.values():
        for beat in doc["beats"]:
            fx = beat["fx"]
            if "bin" in fx and fx["bin"] not in bins:
                bins.append(fx["bin"])
            if fx.get("bgm") and fx["bgm"] not in bgms:
                bgms.append(fx["bgm"])
            for k in ("se", "se1", "se2", "se3"):
                if fx.get(k) and fx[k] not in ses:
                    ses.append(fx[k])
            for c in beat["cast"]:
                if c["name"] and (slug(c["name"]), c["expr"] or 0) not in sprites:
                    sprites.append((slug(c["name"]), c["expr"] or 0))

    # make sure every dependency of our prefab bundles is present before extraction
    have_prefabs = [p.stem for p in BUNDLES.glob("avgpicprefabs_*.ab")]
    ensure_bundles(prefab_dependencies(have_prefabs))

    manifest["img"].update(extract_bgs(profiles, bins))
    manifest["img"].update(extract_sprites(sprites))

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

    def resolve_audio(name):
        cue = tmpl.get(name, name).lower()
        if cue in cues:
            return ("bgm_" if name in bgms else "se_") + name, cues[cue]
        # try to fetch a bank named <cue>.acb and re-extract
        if find_dat_entry(cue + ".acb"):
            fresh = audio_cues(bgms, ses)
            cues.update(fresh)
            if cue in cues:
                return ("bgm_" if name in bgms else "se_") + name, cues[cue]
        return None, None
    for name in bgms + ses:
        key, path = resolve_audio(name)
        if path:
            manifest["aud"][key] = path
        else:
            manifest["warnings"].append(f"audio '{name}' (cue '{tmpl.get(name, name)}') unresolved")

    for k in sorted(manifest["img"]):
        print(k, "->", manifest["img"][k])
    for k in sorted(manifest["aud"]):
        print(k, "->", manifest["aud"][k])
    for w in manifest["warnings"]:
        print("WARN", w)
    Path(f"assets/manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
