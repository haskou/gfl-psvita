#!/usr/bin/env python3
"""Step 02b, part 1: download the Unity bundles needed for story assets.

stdlib-only (urllib), so it runs on the host; extraction runs in docker.
Reads a region resdata JSON (resdata_no_hash.json from gf-data-tools/gf-data-<region>)
and fetches bundles by assetBundleName into assets/bundles/.

Usage:
  python3 tools/fetch_bundles.py --region us --list <pattern>       # find bundles
  python3 tools/fetch_bundles.py --region us name1 name2 ...        # download
"""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

RESDATA_URL = "https://github.com/gf-data-tools/gf-data-{region}/raw/main/resdata_no_hash.json"
BUNDLES_DIR = Path("assets/bundles")


def load_resdata(region: str) -> dict:
    cache = Path(f"assets/bundles/{region}_resdata.json")
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading resdata for {region}...")
        urllib.request.urlretrieve(RESDATA_URL.format(region=region), cache)
    return json.loads(cache.read_text())


def all_bundles(d: dict) -> list[dict]:
    return d["BaseAssetBundles"] + d["AddAssetBundles"] + d.get("passivityAssetBundles", [])


def find(resdata: dict, pattern: str) -> list[dict]:
    return [b for b in all_bundles(resdata) if pattern.lower() in b["assetBundleName"].lower()]


def download(resdata: dict, names: set[str]) -> int:
    base = resdata["resUrl"]
    by_name = {b["assetBundleName"]: b for b in all_bundles(resdata)}
    missing = names - by_name.keys()
    if missing:
        sys.exit(f"unknown bundles: {sorted(missing)}")
    BUNDLES_DIR.mkdir(parents=True, exist_ok=True)
    failed = 0
    for name in sorted(names):
        b = by_name[name]
        dest = BUNDLES_DIR / f"{name}.ab"
        url = base + b["resname"] + ".ab"
        if dest.exists() and dest.stat().st_size == b["sizeCompress"]:
            print(f"ok  {name} (cached)")
            continue
        try:
            urllib.request.urlretrieve(url, dest)
            print(f"got {name} ({dest.stat().st_size} bytes)")
        except Exception as exc:
            failed += 1
            print(f"FAIL {name}: {exc}", file=sys.stderr)
    return failed


def find_dat(resdata: dict, name: str):
    for b in resdata["bytesData"]:
        if b["fileName"].lower() == name.lower():
            return b
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--region", default="us")
    ap.add_argument("--find", help="substring search over bundle names")
    ap.add_argument("--find-dat", help="substring search over bytesData file names")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    resdata = load_resdata(args.region)
    if args.find_dat:
        pat = args.find_dat.lower()
        for b in resdata["bytesData"]:
            if pat in b["fileName"].lower():
                print(b["fileName"], b["sizeCompress"])
        return
    if args.find:
        for b in find(resdata, args.find):
            print(b["assetBundleName"], b["sizeCompress"])
        return
    if not args.names:
        sys.exit("nothing to do")
    # names ending in .acb/.awb refer to bytesData entries, others to bundles
    dats = {n for n in args.names if n.lower().endswith((".acb", ".awb", ".dat"))}
    failed = download(resdata, set(args.names) - dats)
    BUNDLES_DIR.mkdir(parents=True, exist_ok=True)
    for name in sorted(dats):
        entry = find_dat(resdata, name)
        if entry is None:
            print(f"FAIL {name}: not in bytesData", file=sys.stderr)
            failed += 1
            continue
        dest = BUNDLES_DIR / (entry["fileName"] + ".dat")
        url = resdata["resUrl"] + entry["resname"] + ".dat"
        try:
            urllib.request.urlretrieve(url, dest)
            print(f"got {entry['fileName']} ({dest.stat().st_size} bytes)")
        except Exception as exc:
            failed += 1
            print(f"FAIL {name}: {exc}", file=sys.stderr)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
