cmake_minimum_required(VERSION 3.16)
project(gflvn CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gflvn main.cpp)
target_include_directories(gflvn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(VITA)
  # vitasdk: ports install flat libs, no cmake config files
  target_link_libraries(gflvn PRIVATE
      SDL2_image SDL2_mixer SDL2_ttf SDL2
      stdc++
  )
  vita_create_self(gflvn.self gflvn)
  vita_create_vpk(gflvn.vpk GFLVN00001 gflvn.self
      FILE ${VPKDATA}/manifest.json manifest.json
      FILE ${VPKDATA}/scene.ir.json scene.ir.json
      FILE ${VPKDATA}/fonts/NotoSans-Regular.ttf fonts/NotoSans-Regular.ttf
      DIR ${VPKDATA}/img img
      DIR ${VPKDATA}/aud aud
  )
else()
  find_package(SDL2 REQUIRED)
  find_package(SDL2_image REQUIRED)
  find_package(SDL2_mixer REQUIRED)
  find_package(SDL2_ttf REQUIRED)
  target_link_libraries(gflvn PRIVATE
      SDL2::SDL2 SDL2_image::SDL2_image SDL2_mixer::SDL2_mixer SDL2_ttf::SDL2_ttf)
endif()
