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


def test_vita_system_keyboard_services_are_initialized_before_sdl():
    app_util = "sceAppUtilInit(&appUtilInit, &appUtilBoot)"
    dialog = "sceCommonDialogSetConfigParam(&dialogConfig)"
    assert app_util in SOURCE and dialog in SOURCE
    assert SOURCE.index(app_util) < SOURCE.index("CHECK(SDL_Init(")
    assert SOURCE.index(dialog) < SOURCE.index("CHECK(SDL_Init(")
    assert 'SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");' in SOURCE


def test_failed_or_closed_vita_keyboard_cannot_trap_menu_focus():
    assert "searchFocused = SDL_IsScreenKeyboardShown(win);" in SOURCE
    assert "if (!searchFocused)" in SOURCE
    assert "if (searchFocused && !SDL_IsScreenKeyboardShown(win)) closeSearch(false);" in SOURCE
    assert "sceImeDialogAbort();" in SOURCE


def test_story_circle_toggles_clean_view_and_square_opens_history():
    assert "storyUiHidden = !storyUiHidden;" in SOURCE
    assert "if (!storyUiHidden && sayEv" in SOURCE
    assert "if (!storyUiHidden && !showLog && !showScript) drawToolbar();" in SOURCE
    assert "e.cbutton.button == SDL_CONTROLLER_BUTTON_X ||" in SOURCE


def test_story_text_is_two_pixels_larger_without_growing_menu_text():
    assert "TTF_OpenFont(fontPath.c_str(), 20)" in SOURCE
    assert "TTF_OpenFont(fontPath.c_str(), 21)" in SOURCE
    assert "TTF_OpenFont(fontPath.c_str(), 12)" in SOURCE
    assert "std::round(20.0 * value / 50.0)" in SOURCE


if __name__ == "__main__":
    from tests.support import run_module_tests
    run_module_tests(globals())
