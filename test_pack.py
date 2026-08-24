#!/usr/bin/env python3
"""Structural checks for the Vita single-file data pack."""
from pathlib import Path
from unittest import SkipTest

from tools.build_pack import MAX_PACK_SIZE, ROOT, collect_files


def test_pack_inputs_are_complete_and_seekable():
    if not (ROOT / "assets/manifest.json").is_file():
        raise SkipTest("requires a complete local assets import")
    files = collect_files(ROOT / "assets/manifest.json")
    names = [name for name, _ in files]
    assert len(names) == len(set(names))
    assert all(not name.startswith("assets/") for name in names)
    assert any(name.startswith("scenes/") for name in names)
    assert any(name.startswith("img/") for name in names)
    assert any(name.startswith("aud/") for name in names)
    total = sum(path.stat().st_size for _, path in files)
    assert total < MAX_PACK_SIZE, f"pack is too large for Vita fseek: {total}"


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
