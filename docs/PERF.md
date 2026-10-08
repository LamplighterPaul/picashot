# Performance

How fast Picashot is, how that is measured, and what was tried.

## Measuring

Two tools ship with the source.

**The built-in profiler.** Run `PICASHOT_PROFILE=1 picashot` and every two seconds it prints, for that
window: camera frame rate; the median, 99th percentile and worst time of each stage on the CPU and on
the GPU (GPU stages use Vulkan timestamp queries); latency from a frame reaching the process to its
submission to the GPU, and from the camera starting the frame; CPU use per thread; the ffmpeg child's
CPU while recording; resident, peak and GPU memory. It also prints the time from process start to the
first picture.

**`tools/bench [label] [binary]`.** Runs the same scenarios every time (20 s preview, 45 s recording
with sound on the Noir filter, 20 s silent recording, three photos), reads the profiler output and
prints one table. It also counts user-space instructions per second for the app and for ffmpeg with
`tools/cyclecount`. Instruction counts are the number to compare between builds: "percent of a core"
moves by a fifth from run to run on a laptop on battery, because the CPU changes speed.

## Results, 8 October 2026

Lenovo Yoga Slim, Intel Core Ultra 7 355, integrated graphics, on battery. Camera: 1920x1080 MJPEG at
30 fps over USB. Window on a hidden workspace. "Before" is the program as first written (SDL camera,
RGBA everywhere, software x264); "after" is the current code. Ranges are the spread over two or three runs.

| | Before | After |
|---|---|---|
| **Preview** | | |
| App work, million instructions per second | 3115 | 1600 to 1612 |
| App CPU, percent of one core | 26 to 32 | 14 to 18 |
| Latency, frame received to GPU, median | 9.7 to 11.9 ms | 5.4 to 6.0 ms |
| GPU time, window pass, median | 3.3 to 4.2 ms | 1.4 to 2.2 ms |
| Peak memory | 154 MB | 76 MB |
| **Recording with sound (Noir)** | | |
| App work, million instructions per second | 3095 | 1603 to 1674 |
| ffmpeg work, million instructions per second | 8821 | 189 to 261 |
| App CPU, percent of one core | 33 to 41 | 19 to 22 |
| ffmpeg CPU, percent of one core | 85 to 120 | 15 to 17 |
| Main loop time per recorded frame, median | 10.6 to 11.3 ms | 0.2 to 0.3 ms |
| Latency, frame received to GPU, median | 21 to 23 ms | 5.4 to 5.7 ms |
| Peak memory | 187 MB | 78 MB |
| Dropped frames in 45 s | 0 | 0 |
| **Start-up** | | |
| Process start to first picture | 290 to 400 ms | 255 to 370 ms |

Instruction counts cover user space only. ffmpeg with a GPU encoder does most of its remaining work in
the kernel and the driver, so its CPU percentage is the fairer figure for it.

## What changed

1. **Own camera capture.** Picashot reads the camera through Video4Linux2 itself. MJPEG frames are
   decoded by libjpeg-turbo straight to YUV planes on a capture thread; the GPU converts to RGB. SDL's
   camera path, which decodes with a slower decoder and converts to RGBA on the CPU, remains as a
   fallback. This halved the preview's work.
2. **GPU video encoder.** At the first camera frame a background thread tests the encoders ffmpeg
   offers (VA-API, NVENC, V4L2 M2M, then software x264 and OpenH264) with a tiny clip and keeps the
   first that works.
3. **NV12 to the encoder.** The filtered picture is converted to NV12 on the GPU, so 3.1 MB per frame
   crosses to ffmpeg, not 8.3 MB, and ffmpeg has no colour conversion to do.
4. **No waiting for the GPU.** A capture is collected when the GPU has finished it; the main loop
   used to block for it on every recorded frame.
5. **Event-driven loop.** The loop sleeps until an input event or a camera frame, where it used to wake
   every 2 ms. A change to the interface alone redraws the window without running the filter again.
6. **Cheaper interface shader.** The buttons, dots and digits are only computed near where they are.

## What was measured and not adopted

- **GPU JPEG decoding through VA-API.** Decoding costs 1.1 ms of CPU per frame if the picture stays on
  the GPU, but 19.6 ms once it is downloaded to memory, against 4.9 ms for software decoding. It only
  pays off with zero-copy import of the decoder's surfaces into Vulkan (dma-buf with DRM format
  modifiers), which is driver-specific. Not done; see below.
- **libjpeg-turbo's fast DCT.** No measurable difference.
- **Vulkan Video encoding.** Not supported by this machine's driver.
- **The laptop's second camera node** (`/dev/video50`, the IPU7 loopback) offers raw NV12 but does not stream.

## Where the time goes now

Almost all of the app's remaining user-space work is JPEG decoding: 1660 of 1667 million instructions
per second are in the camera thread, about 55 million per 1080p frame. Entropy decoding of a
high-quality JPEG is serial, so software is at its floor here.

GPU cost per frame on this machine at 1080p: 0.2 to 0.4 ms for single-sample filters, about 0.7 ms
for Comic (33 samples per pixel), 1.4 ms for Dream (17), 2 ms for the grid of all sixteen.

## Next candidates

- Zero-copy hardware JPEG decode (VA-API surface exported as dma-buf, imported into Vulkan). Expected
  to cut preview CPU to roughly a third of today's on Intel and AMD. Needs Vulkan extensions beyond
  1.0 and stays optional.
- Noir's film grain makes video hard to compress: 36 Mbit/s at 1080p against about 8 for a plain
  picture. A rate cap or coarser grain would shrink those files.
- Start-up is dominated by the camera starting to stream; a pipeline cache would trim the rest.
- Not yet measured on ARM or on a discrete GPU.
