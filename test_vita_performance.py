#!/usr/bin/env python3
"""Regression checks for storage-friendly Vita audio playback."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = (ROOT / "runtime/main.cpp").read_text()


def test_packed_music_is_buffered_before_mixer_streaming():
    assert "std::vector<uint8_t> musicBuffer;" in SOURCE
    assert "SDL_RWFromConstMem(musicBuffer.data()" in SOURCE
    assert "Mix_LoadMUS_RW(memory, 1)" in SOURCE
    assert "Mix_LoadMUS_RW(packed" not in SOURCE


def test_music_buffer_outlives_and_is_released_after_the_track():
    stop_body = SOURCE.split("void Player::stopMusic()", 1)[1].split(
        "void Player::playMusic", 1
    )[0]
    assert stop_body.index("Mix_FreeMusic(curMusic)") < stop_body.index(
        "musicBuffer.clear()"
    )


def test_audio_buffer_absorbs_short_image_decode_spikes():
    assert "Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096)" in SOURCE


if __name__ == "__main__":
    from tests.support import run_module_tests

    run_module_tests(globals())
