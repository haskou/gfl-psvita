"""Small test runner shared by the standalone test modules."""

from __future__ import annotations

from collections.abc import Mapping
from unittest import SkipTest


def run_module_tests(namespace: Mapping[str, object]) -> None:
    passed = 0
    skipped = 0
    for name, candidate in sorted(namespace.items()):
        if not name.startswith("test_") or not callable(candidate):
            continue
        function = candidate
        try:
            function()
        except SkipTest as exc:
            skipped += 1
            print(f"SKIP {name}: {exc}")
        else:
            passed += 1
            print(f"PASS {name}")
    print(f"summary: {passed} passed, {skipped} skipped")
