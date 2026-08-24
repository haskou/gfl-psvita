#!/usr/bin/env python3
"""Prepare user-supplied event posters/logos for Vita's 960x544 UI."""
from pathlib import Path
import re
import sys

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parent.parent
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "Downloads/GFL_Eventos 2"
OUT = ROOT / "assets/img"


def slug(value: str) -> str:
    value = re.sub(r"^\d+\s*-\s*", "", value).casefold()
    return re.sub(r"[^a-z0-9]+", "_", value).strip("_")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for folder in sorted(p for p in SOURCE.iterdir() if p.is_dir()):
        key = slug(folder.name)
        logo = next(folder.glob("logo.*"), None)
        poster = next(folder.glob("poster.*"), None)
        if not logo or not poster:
            continue
        with Image.open(poster) as source:
            # A wallpaper must fill Vita's native framebuffer. ImageOps.fit
            # retains aspect ratio and crops symmetrically, never stretches.
            fitted = ImageOps.fit(source.convert("RGB"), (960, 544), method=Image.Resampling.LANCZOS)
            fitted.save(OUT / f"event_{key}_poster.jpg", quality=88, optimize=True, progressive=True)
        with Image.open(logo) as source:
            image = source.convert("RGBA")
            image.thumbnail((300, 150), Image.Resampling.LANCZOS)
            canvas = Image.new("RGBA", (300, 150), (0, 0, 0, 0))
            canvas.alpha_composite(image, ((300 - image.width) // 2, (150 - image.height) // 2))
            canvas.save(OUT / f"event_{key}_logo.png", optimize=True)
        count += 1
    print(f"event artwork: {count} logo/poster pairs")


if __name__ == "__main__":
    main()
