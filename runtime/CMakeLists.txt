cmake_minimum_required(VERSION 3.16)
project(gflvn CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gflvn main.cpp)
target_include_directories(gflvn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(VITA)
  include("${VITASDK}/share/vita.cmake" REQUIRED)
  # vitasdk: ports install flat libs + headers under include/<lib>, no cmake configs
  target_include_directories(gflvn PRIVATE
      $ENV{VITASDK}/arm-vita-eabi/include/SDL2
      $ENV{VITASDK}/arm-vita-eabi/include/SDL2_image
      $ENV{VITASDK}/arm-vita-eabi/include/SDL2_mixer
      $ENV{VITASDK}/arm-vita-eabi/include/SDL2_ttf)
  set(VITA_LIBS
      SDL2_image SDL2_ttf SDL2_mixer SDL2
      webpdemux webpmux webp jpeg png16 z bz2
      freetype harfbuzz
      modplug mpg123 xmp opusfile opus vorbisfile vorbis FLAC ogg
      vita2d
      SceGxm_stub SceDisplay_stub SceAppMgr_stub SceAppUtil_stub
      SceTouch_stub SceCommonDialog_stub SceAudio_stub SceCtrl_stub SceSysmodule_stub
      SceAudioIn_stub SceHid_stub SceMotion_stub
      pthread m stdc++)
  target_link_libraries(gflvn PRIVATE -Wl,--start-group ${VITA_LIBS} -Wl,--end-group)
  vita_create_self(gflvn.self gflvn)
  # vpk packaging happens in tools/build_vita.sh (vita-pack-vpk handles dirs)
else()
  find_package(SDL2 REQUIRED)
  find_package(SDL2_image REQUIRED)
  find_package(SDL2_mixer REQUIRED)
  find_package(SDL2_ttf REQUIRED)
  target_link_libraries(gflvn PRIVATE
      SDL2::SDL2 SDL2_image::SDL2_image SDL2_mixer::SDL2_mixer SDL2_ttf::SDL2_ttf)
endif()
