#!/usr/bin/env python3
"""Regression checks for Vita front/rear touch separation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = (ROOT / "runtime/main.cpp").read_text()


def test_rear_touch_is_disabled_before_sdl_initializes():
    disable = 'SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);'
    assert disable in SOURCE
    assert SOURCE.index(disable) < SOURCE.index("CHECK(SDL_Init(")


def test_touch_does_not_also_generate_mouse_clicks():
    assert 'SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");' in SOURCE


def test_only_the_front_touch_id_is_actionable():
    assert "event.tfinger.touchId == 1" in SOURCE
    assert SOURCE.count("SDL_FINGERDOWN") == 1


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
