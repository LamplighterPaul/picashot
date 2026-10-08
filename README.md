# Picashot

A photo booth for Linux. Launch it and your camera is on screen. Press Space for a photo or V for a
video, or use the two buttons at the bottom, and pick from sixteen live filters.

Picashot is written in C with SDL3 and Vulkan. It is plain C17 with nothing tied to one CPU
architecture, and it uses only Vulkan 1.0 features, so it is meant for x86_64 and aarch64 and for any
GPU with a Vulkan driver. See [Status](#status) for what has been tested so far.

## Keys

| Key | Action |
|---|---|
| Space or Enter | Take a photo (3-second countdown; press again to cancel) |
| V or R | Start or stop a video |
| S | Sound for videos on or off (also the microphone button) |
| Left / Right, mouse wheel | Previous or next filter |
| 1 to 9 | Jump to a filter |
| Tab or G, double-click | Show every filter live in a grid; click or press Enter to choose |
| M | Mirror the picture on or off |
| T | Countdown timer on or off (also the "3" button) |
| C | Next camera |
| F or F11 | Fullscreen |
| Esc or Q | Quit |

Photos are saved as PNG in `Pictures/Picashot` and videos as MP4 in `Videos/Picashot`. What you see
is what is saved: the filtered picture at the camera's full resolution, mirrored if the preview is.

Videos record the microphone unless you turn sound off. The microphone button shows which, and the
microphone is only open while a recording runs.

## Install

**Arch, Omarchy and other Arch-based systems:** build the package from this repository:

```sh
sudo pacman -S --needed sdl3 shaderc libjpeg-turbo
git clone https://github.com/LamplighterPaul/picashot.git
cd picashot/packaging && makepkg -si
```

**Prebuilt binary (x86_64):** each [release](https://github.com/LamplighterPaul/picashot/releases)
has a tarball with the program, its launcher entry and icon. It needs SDL3 3.4 or newer, libjpeg-turbo
and a recent glibc on the system, so it suits rolling and recent distributions.

```sh
tar xf picashot-0.1-linux-x86_64.tar.gz
cd picashot-0.1-linux-x86_64 && ./install.sh        # into ~/.local; ./install.sh --remove undoes it
```

## Build

You need a C compiler, SDL3 (3.4 or newer), `glslc` from shaderc, and libjpeg-turbo (optional, but
without it most webcams go through a slower path). On Arch or Omarchy:

```sh
sudo pacman -S --needed sdl3 shaderc libjpeg-turbo
make
./picashot
```

To install it as an Arch package, with its launcher entry and icon:

```sh
cd packaging && makepkg -si
```

Or without a package manager: `sudo make install` (uses `/usr/local`; set `PREFIX` to change it).

Optional at run time:

- `ffmpeg`, to record videos. Picashot uses a GPU encoder when ffmpeg has one that works (VA-API,
  NVENC or V4L2 M2M) and software x264 otherwise. Sound comes from the default PulseAudio or PipeWire input.
- `notify-send` (libnotify), for a notification when a file is saved.
- `vulkan-swrast` (lavapipe), which should let it run on a machine with no Vulkan GPU driver.

## Command line

```
picashot --filter comic          start with a filter
picashot --camera 1              use the second camera (see --list-cameras)
picashot --size 1280x720         prefer a smaller camera mode
picashot --snap 2                take a photo after 2 seconds and exit
picashot --record 10             record 10 seconds and exit
picashot --no-audio              start with sound off for videos
picashot --encoder x264          force a video encoder (vaapi, nvenc, v4l2m2m, x264, openh264)
```

`picashot --help` lists the rest. `PICASHOT_GPU=N` picks a Vulkan device by number when the machine
has several; by default the integrated GPU is used.

## Performance

On a Lenovo Yoga Slim (Intel Core Ultra 7 355, integrated graphics, on battery) with a 1080p 30 fps
USB webcam:

| | |
|---|---|
| Launch to first picture | 0.26 to 0.37 s |
| Live preview | 14 to 18% of one CPU core, 76 MB of memory |
| Recording 1080p with sound | 19 to 22% of one core for Picashot, 15 to 17% for the encoder |
| Frame received to on its way to the screen | 5.5 ms (median) |
| Frames dropped in a 45-second recording | 0 |
| GPU time per frame | 0.2 to 0.4 ms for most filters, 1.4 ms for the heaviest |
| Program size | 166 KB stripped |

How these are measured, and the before-and-after of the optimisation work, are in
[docs/PERF.md](docs/PERF.md). `PICASHOT_PROFILE=1 picashot` prints the same measurements on your machine.

## Writing a filter

A filter is one small GLSL file. See [docs/FILTERS.md](docs/FILTERS.md). `make dev` builds and runs a
developer binary with shader hot reload: edit a filter, run `make DEV=1 shaders`, and the live preview
changes without a restart.

## How it works

- `src/main.c`: window, keys, buttons and the main loop.
- `src/camera_v4l2.c`: reads the camera directly. MJPEG is decoded straight to YUV planes on a
  capture thread. `src/camera_sdl.c` is the fallback through SDL, for cameras only PipeWire can reach.
- `src/gpu.c`: the Vulkan renderer. Camera planes are converted to RGB, run through the filter shader
  into a picture the size of the camera frame, and shown in the window. A photo is that picture copied
  back as RGBA; a video frame is converted to NV12 on the GPU first.
- `src/capture.c`: saves photos on a background thread and feeds video frames to an `ffmpeg` process.
- `src/profile.c`: the built-in profiler (`PICASHOT_PROFILE=1`).
- `src/filters.h`: the list of filters.
- `shaders/`: one file per filter, plus the renderer's own: `present.frag` draws the interface.

Only Vulkan 1.0 features are used, so older and embedded drivers work. Performance numbers and how
they are measured are in [docs/PERF.md](docs/PERF.md).

## Status

Version 0.1, the first public release. Tested on one machine: x86_64, Intel integrated graphics with VA-API,
a USB webcam sending MJPEG, Hyprland on Wayland. Not yet tested: an aarch64 build, the lavapipe
software driver, NVENC and V4L2 M2M encoders, cameras that are only reachable through PipeWire or
libcamera, and X11. Reports from other hardware are very welcome.

## Licence

MIT. See [LICENSE](LICENSE). The Vulkan headers in `third_party/vulkan` are from Khronos, under
Apache-2.0 or MIT.
