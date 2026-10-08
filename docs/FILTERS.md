# Writing a filter

A filter is a fragment shader that turns the camera picture into something else. Each one is a single
file in `shaders/`.

## The smallest filter

`shaders/negative.frag`:

```glsl
#version 450
#include "common.glsl"

vec3 effect(vec2 uv) {
    return 1.0 - tex(uv);
}
```

`effect` is called once per pixel. `uv` is where that pixel is in the picture: `(0,0)` is the top-left
corner and `(1,1)` the bottom-right. Return the colour for that pixel as red, green and blue in `0..1`.

Then add one line to `FILTER_LIST` in `src/filters.h`:

```c
    X(negative, "Negative") \
```

The first name is the file name, the second is the title shown in the window. The position in the
list is the position on screen. The grid is four tiles wide, and a full grid keeps every tile the same
shape as the camera picture, so the list works best with 16 entries.

Run `make` and the filter is in.

## What `common.glsl` gives you

| Name | Meaning |
|---|---|
| `tex(uv)` | The camera colour at `uv` |
| `RES` | Camera size in pixels, for example `vec2(1920, 1080)`. `1.0 / RES` is one pixel |
| `TIME` | Seconds since launch, for filters that move |
| `luma(c)` | Brightness of a colour |
| `hash(p)` | A repeatable random number in `0..1` for a position |
| `vignette(uv, amount)` | A factor that darkens the corners |

Mirroring is handled for you before `effect` runs. Colours are the camera's own sRGB values; no
conversion is applied, which is what most photo filters expect.

## Kinds of filter, with examples to copy

- **Colour** changes each pixel by itself: `sepia.frag`, `thermal.frag`, `xray.frag`.
- **Warp** reads the camera from a different place: `bulge.frag`, `twirl.frag`, `kaleido.frag`.
- **Neighbourhood** reads several nearby pixels: `comic.frag` (edges), `dream.frag` (glow).
- **Pattern** rebuilds the picture from shapes: `halftone.frag`, `pixel.frag`.

## Hot reload

```sh
make dev
```

This builds the developer binary and runs it watching `build/dev/spv`. Edit a filter, run
`make DEV=1 shaders` in another terminal, and the preview switches to the new version in about a third
of a second. If the shader does not compile, the command prints the error and the running preview
keeps the old version. Release builds do not have hot reload.

A filter added to `FILTER_LIST` needs a full `make dev` and a restart the first time; after that it
hot reloads like the rest.

## Rules of thumb

- A filter runs for every pixel of a 1080p picture, 30 times a second, on integrated and ARM GPUs
  too. Keep texture reads to a few dozen per pixel.
- Use only GLSL 4.50 core features. Picashot targets Vulkan 1.0 so that it runs everywhere.
