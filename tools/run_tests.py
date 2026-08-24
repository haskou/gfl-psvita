#!/usr/bin/env python3
"""Run every dependency-free test module in an isolated process."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    failed: list[str] = []
    tests = sorted(ROOT.glob("test_*.py"))
    for test in tests:
        print(f"\n==> {test.name}", flush=True)
        result = subprocess.run([sys.executable, str(test)], cwd=ROOT, check=False)
        if result.returncode:
            failed.append(test.name)
    if failed:
        print(f"\nFAIL: {', '.join(failed)}", file=sys.stderr)
        return 1
    print(f"\nPASS: {len(tests)} test modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
