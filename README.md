# DQ8 Model Viewer

A viewer for the character and monster models of **Dragon Quest VIII: Journey of
the Cursed King** (PlayStation 2). It reads the models, textures and animations
straight from your own copy of the game disc (an `.iso` image) and plays them
back in real time.

- Every `.chr` model pack on the disc: the hero, party members, NPCs, monsters,
  weapons and props
- Skeletal animations, including retargeted ones (party members share motions)
- Textures, texture animations (blinking eyes, mouths), transparency, additive
  effects (weapon trails, flares) and environment-mapped shine (metal slimes)
- Orbit/zoom camera, frame stepping, search, and BMP screenshots

It is written in C, with a software rasterizer and [SDL3](https://libsdl.org)
for the window and input.

> **No game data is included.** You need your own dump of the game. The viewer
> was developed against the European release (`SLES-53974`, *Dragon Quest - The
> Journey of the Cursed King*). Other releases may work but are untested.

## Download

Prebuilt Windows and Linux builds are on the
[Releases](https://github.com/CodeTappert/DQ8-Modelviewer/releases) page. Unpack
the archive and run `dq8-model-viewer` (`dq8-model-viewer.exe` on Windows).

## Usage

On first start the viewer asks for the game ISO with a file dialog and remembers
your choice. You can also pass it on the command line:

```
dq8-model-viewer [filter] [--iso <path to game.iso>] [--anim N]
```

| Argument | Meaning |
|---|---|
| `filter` | Only list models whose path contains this text (default: `chara/`). Use `""` for every model on the disc, e.g. `item/` for weapons. |
| `--iso <path>` | Use this ISO (and remember it). |
| `--anim N` | Start with animation `N` (`-1` = T-pose). |
| `--autoshot <file.bmp>` | Render a few frames, save the last one and quit. |

The ISO is looked up in this order: `--iso`, the `DQ8_ISO` environment variable
(or `DQ8_DATA_DIR` for an extracted disc), the last ISO you opened, and finally
the file dialog. To pick a different ISO later, start the viewer with `--iso`.

### Controls

Press **F1** in the window to show them.

| Key | Action |
|---|---|
| Up / Down | Previous / next model |
| Page Up / Page Down | Jump 20 models |
| Home / End | First / last model |
| Left / Right | Previous / next animation (the first entry is the T-pose) |
| Space | Play / pause |
| `,` / `.` | Step one frame back / forward |
| Mouse drag | Orbit the camera |
| Right or middle drag, Shift + drag | Move the model on screen |
| Mouse wheel | Zoom |
| 1 / 2 / 3 | Front / side / three-quarter view |
| R | Reset the camera |
| `/` | Search models (type, then Enter) |
| T | Texture animation on / off |
| S | Save a screenshot to `screenshots/` (BMP) |
| Esc | Close search / quit |

## Building

You need CMake 3.24+ and a C11 compiler (GCC, Clang, MinGW or MSVC). If SDL3 is
installed, CMake uses it. Otherwise CMake downloads SDL 3.4.0 and links it
statically, which needs `git` and an internet connection on the first configure.

### Linux

Install the libraries SDL needs for X11/Wayland (Debian/Ubuntu names):

```bash
sudo apt install build-essential cmake ninja-build git pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libdbus-1-dev libudev-dev libdecor-0-dev
```

Then build:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

The binary is `build/dq8-model-viewer`. The file dialog uses the XDG desktop
portal (or `zenity`), which most desktops provide.

### Windows

With Visual Studio 2022 (Developer PowerShell), or with MinGW-w64 and Ninja:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --config Release
```

The binary is `build/Release/dq8-model-viewer.exe` (Visual Studio) or
`build/dq8-model-viewer.exe` (Ninja). To use an SDL3 you already have (for
example from vcpkg), pass `-DCMAKE_PREFIX_PATH=<sdl3 prefix>`.

## Project layout

| Path | Contents |
|---|---|
| `src/dq8/io/` | Disc access: ISO9660, the `DATA.HD6`/`DATA.DAT` archive, `.pak`/`.img` containers, TIM2 textures, texture animation scripts |
| `src/dq8/model/` | `.mds` models (meshes, skeleton, skinning, materials) and `.mot` motions |
| `src/platform/` | SDL3 window, input, timing and file dialog |
| `tools/common/` | Model-pack loading (pairing bodies, motions, weapons and textures) and the software rasterizer |
| `tools/viewer/` | The viewer itself |

The decoders come from an ongoing reverse-engineering and reimplementation
project for Dragon Quest VIII.

## Known limitations

- Particle effects and some effect props are not shown.
- Cloth (`.clo`) simulation is not implemented.
- Texture-animation commands other than copy and scroll are ignored.

## Legal

This project is not affiliated with or endorsed by Square Enix or Level-5.
Dragon Quest is a trademark of Square Enix. The viewer contains no game code or
assets. It only reads data from a disc image you provide.

Licensed under the [MIT License](LICENSE).

## AI usage

This project was written with the help of AI coding assistants. All changes are reviewed and tested by the maintainer.
