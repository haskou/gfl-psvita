#!/usr/bin/env python3
"""Validate the Vita shell metadata and fixed-size LiveArea artwork."""
import pathlib
import re
import struct


ROOT = pathlib.Path(__file__).parent
SCE_SYS = ROOT / "runtime/sce_sys"


def png_info(path):
    data = path.read_bytes()[:26]
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"not a PNG: {path}"
    width, height = struct.unpack(">II", data[16:24])
    return width, height, data[25]


def test_livearea_artwork():
    expected = {
        SCE_SYS / "icon0.png": (128, 128),
        SCE_SYS / "livearea/contents/bg.png": (840, 500),
        SCE_SYS / "livearea/contents/startup.png": (280, 158),
    }
    for path, dimensions in expected.items():
        width, height, color_type = png_info(path)
        assert (width, height) == dimensions, f"{path}: {(width, height)} != {dimensions}"
        assert color_type == 3, (
            f"{path}: Vita shell artwork must be an indexed PNG, color type={color_type}"
        )


def test_livearea_metadata():
    template = (SCE_SYS / "livearea/contents/template.xml").read_text()
    assert '<livearea style="vd"' in template  # lower-left gate, clear of the cast
    assert "<image>bg.png</image>" in template
    assert "<gate>" in template and "<startup-image>startup.png</startup-image>" in template
    assert "<frame" not in template
    build = (ROOT / "tools/build_vita.sh").read_text()
    assert '"Girl\'s Frontline" /build/param.sfo' in build
    assert 'runtime/OFL.txt runtime/APACHE-2.0.txt' in build
    title_id = re.search(r"TITLE_ID=([A-Z0-9]+)", build).group(1)
    assert title_id == "GFLVN0001" and len(title_id) == 9


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
