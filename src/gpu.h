// The Vulkan side: camera frame in, filtered picture out to the window and, on request, back to memory.
#ifndef PICASHOT_GPU_H
#define PICASHOT_GPU_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera.h"

#define GPU_MAX_CAMERA_SIDE 8192

typedef struct {
    float time;         // seconds since launch
    int   filter;       // index into g_filters
    bool  mirror;
    int   grid_cols;    // 0 = one filter full size; otherwise show every filter in a grid this wide
    int   hover;        // hovered grid tile, -1 for none
    float flash;        // 0..1 white flash
    float countdown;    // seconds left, <= 0 when off
    float rec_seconds;  // recording time, < 0 when not recording
    int   hover_button; // on-screen button under the pointer: 0 none, 1 photo, 2 video, 3 timer, 4 sound
    bool  sound_on;     // videos record the microphone
    bool  timer_on;     // photos use the countdown
} GpuFrame;

enum { GPU_CAPTURE_PHOTO = 1, GPU_CAPTURE_VIDEO = 2 };

bool gpu_init(SDL_Window *window);
void gpu_shutdown(void);
const char *gpu_device_name(void);
bool gpu_ok(void);                      // false once the device has failed for good

// (Re)creates everything sized by the camera mode and shows black until the first frame. If it fails,
// whatever was in use before stays in use.
bool gpu_set_camera_format(const CamFormat *fmt);
// Takes a camera frame for the next gpu_frame. Changes the camera format first if the frame's differs.
bool gpu_upload(const CamFrame *frame);
int  gpu_camera_width(void);
int  gpu_camera_height(void);
int  gpu_video_width(void);             // size of the NV12 video frames (the camera size made even)
int  gpu_video_height(void);

// Draws one frame. `refilter` runs the filter again (needed when the filter, its settings or the time
// it animates on changed); without it only the window is redrawn over the last picture. `capture` is
// a mix of GPU_CAPTURE_* bits: those copies are collected later with gpu_collect.
bool gpu_frame(const GpuFrame *f, bool refilter, int capture);

// The GPU_CAPTURE_* bits submitted and not yet collected. They must be collected before the next gpu_frame.
int  gpu_capture_pending(void);
// Returns true when the pending capture is ready, and points at its pixels: RGBA of the camera size for
// a photo, NV12 of the video size for a video frame (NULL for the one not asked for). The memory is
// valid until the next gpu_frame. With `wait` it blocks until the GPU has finished.
bool gpu_collect(bool wait, const uint8_t **photo_rgba, const uint8_t **video_nv12);

void gpu_window_resized(void);
// Replaces one filter's shader while running (used by shader hot reload).
bool gpu_reload_filter(int index, const uint32_t *spv, size_t bytes);

#endif
