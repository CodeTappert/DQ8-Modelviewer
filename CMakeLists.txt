cmake_minimum_required(VERSION 3.24)
project(dq8_model_viewer VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SDL3: an installed SDL3 (find_package) is used if present, otherwise SDL3 is
# downloaded and linked statically. -DDQ8_FETCH_SDL=ON forces the download.
option(DQ8_FETCH_SDL "Always build SDL3 from source" OFF)
if(NOT DQ8_FETCH_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(SDL3)
  message(STATUS "SDL3: building from source (static)")
endif()

add_executable(dq8-model-viewer
  src/dq8/io/archive.c
  src/dq8/io/file.c
  src/dq8/io/img.c
  src/dq8/io/pak.c
  src/dq8/io/texanime.c
  src/dq8/io/tim2.c
  src/dq8/io/vfs.c
  src/dq8/model/mds.c
  src/dq8/model/mot.c
  src/platform/platform_sdl3.c
  tools/common/scene.c
  tools/common/softrast.c
  tools/viewer/viewer.c)
target_include_directories(dq8-model-viewer PRIVATE include tools)
target_link_libraries(dq8-model-viewer PRIVATE SDL3::SDL3)
set(DQ8_VERSION "dev" CACHE STRING "Version shown in the window title")
target_compile_definitions(dq8-model-viewer PRIVATE DQ8_VERSION="${DQ8_VERSION}" DQ8_SDL_MAIN=1)

if(MSVC)
  target_compile_options(dq8-model-viewer PRIVATE /W3 /utf-8)
  target_compile_definitions(dq8-model-viewer PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(dq8-model-viewer PRIVATE -Wall -Wextra -Wno-format-truncation)
  target_link_libraries(dq8-model-viewer PRIVATE m)
endif()
if(WIN32)
  set_target_properties(dq8-model-viewer PROPERTIES WIN32_EXECUTABLE ON)   # no console window
  if(MINGW)
    target_link_options(dq8-model-viewer PRIVATE -static)                 # no MinGW runtime DLLs
  endif()
endif()

install(TARGETS dq8-model-viewer RUNTIME DESTINATION bin)
