#!/usr/bin/env python3
"""Build the uncompressed, seekable Vita data container.

files. The official VitaSDK packager then adds this single container to the
VPK. After installation the runtime seeks directly to each PNG, OGG or scene
JSON without a first-run extraction.
"""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAX_PACK_SIZE = 2_000_000_000  # Vita libc fseek uses a signed long.


def logical_name(source: Path) -> str:
    rel = source.relative_to(ROOT).as_posix()
    return rel[7:] if rel.startswith("assets/") else rel


def collect_files(manifest_path: Path):
    manifest = json.loads(manifest_path.read_text())
    sources = [ROOT / "assets/scenes" / f"{sid}.ir.json" for sid in manifest["scenes"]]
    sources += [ROOT / rel for rel in manifest["img"].values()]
    sources += [ROOT / rel for rel in manifest["aud"].values()]
    by_name = {}
    for source in sources:
        if not source.is_file():
            raise FileNotFoundError(source)
        name = logical_name(source)
        previous = by_name.setdefault(name, source)
        if previous != source:
            raise RuntimeError(f"pack name collision: {name}: {previous} / {source}")
    return sorted(by_name.items())


def build_pack(destination: Path, manifest_path=ROOT / "assets/manifest.json"):
    destination.mkdir(parents=True, exist_ok=True)
    pack_tmp = destination / "data.gfpak.tmp"
    pack_path = destination / "data.gfpak"
    files = {}
    offset = 0
    with pack_tmp.open("wb") as output:
        for name, source in collect_files(Path(manifest_path)):
            size = source.stat().st_size
            if offset + size >= MAX_PACK_SIZE:
                raise RuntimeError(f"data.gfpak would exceed Vita's safe 2 GB seek limit at {name}")
            files[name] = [offset, size]
            with source.open("rb") as input_file:
                shutil.copyfileobj(input_file, output, length=1024 * 1024)
            offset += size
    pack_tmp.replace(pack_path)
    index = {"version": 1, "size": offset, "files": files}
    (destination / "pack_index.json").write_text(
        json.dumps(index, separators=(",", ":")), encoding="utf-8"
    )
    print(f"pack: {len(files)} files -> {pack_path} ({offset} bytes)")


if __name__ == "__main__":
    build_pack(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/pack")
