// Fallback camera backend: SDL3's camera API, asked for RGBA.
#include <SDL3/SDL.h>

#include "camera.h"

static struct {
    SDL_CameraID *ids;
    int           count;
    bool          listed;
    SDL_Camera   *cam;
    SDL_Surface  *frame, *converted;
    char          mode[96];
} s;

static int sdl_count(void) {
    if (!s.listed) { s.ids = SDL_GetCameras(&s.count); s.listed = true; }
    return s.ids ? s.count : 0;
}

static const char *sdl_name(int index) {
    const char *name = index >= 0 && index < sdl_count() ? SDL_GetCameraName(s.ids[index]) : NULL;
    return name ? name : "camera";
}

static bool sdl_open(int index, int want_w, int want_h) {
    if (index < 0 || index >= sdl_count()) return false;
    SDL_CameraSpec spec = { 0 };
    bool found = false;
    long best = -1;
    int n = 0;
    SDL_CameraSpec **specs = SDL_GetCameraSupportedFormats(s.ids[index], &n);
    for (int i = 0; specs && i < n; i++) {
        double fps = specs[i]->framerate_denominator ? (double)specs[i]->framerate_numerator / specs[i]->framerate_denominator : 0;
        long score = camera_score_mode(specs[i]->width, specs[i]->height, fps, want_w, want_h);
        if (score > best) { best = score; spec = *specs[i]; found = true; }
    }
    SDL_free(specs);
    // Ask for RGBA whatever the camera's own format is; SDL converts on its camera thread.
    spec.format = SDL_PIXELFORMAT_RGBA32;
    spec.colorspace = SDL_COLORSPACE_SRGB;
    s.cam = SDL_OpenCamera(s.ids[index], found ? &spec : NULL);
    if (!s.cam) return false;
    if (found) SDL_snprintf(s.mode, sizeof s.mode, "%dx%d via SDL, %d fps", spec.width, spec.height,
                            spec.framerate_denominator ? spec.framerate_numerator / spec.framerate_denominator : 0);
    else SDL_strlcpy(s.mode, "SDL default mode", sizeof s.mode);
    return true;
}

static void sdl_release(void) {
    if (s.converted) SDL_DestroySurface(s.converted);
    if (s.frame && s.cam) SDL_ReleaseCameraFrame(s.cam, s.frame);
    s.converted = s.frame = NULL;
}

static void sdl_close(void) {
    sdl_release();
    if (s.cam) SDL_CloseCamera(s.cam);
    s.cam = NULL;
}

static bool sdl_acquire(CamFrame *out) {
    if (!s.cam) return false;
    Uint64 stamp = 0;
    s.frame = SDL_AcquireCameraFrame(s.cam, &stamp);
    if (!s.frame) return false;
    SDL_Surface *rgba = s.frame;
    if (rgba->format != SDL_PIXELFORMAT_RGBA32) rgba = s.converted = SDL_ConvertSurface(s.frame, SDL_PIXELFORMAT_RGBA32);
    if (!rgba || rgba->w <= 0 || rgba->h <= 0 || rgba->pitch < rgba->w * 4) { sdl_release(); return false; }
    *out = (CamFrame){ .fmt = { .w = rgba->w, .h = rgba->h, .layout = CAM_RGBA }, .plane = { rgba->pixels },
                       .pitch = { rgba->pitch } };
    // SDL stamps frames on its own clock, which counts from start-up; shift it onto CLOCK_MONOTONIC.
    Uint64 age = stamp && SDL_GetTicksNS() > stamp ? SDL_GetTicksNS() - stamp : 0;
    out->stamp_ns = out->arrived_ns = camera_monotonic_ns() - age;   // SDL stamps a frame when it takes it from the driver
    return true;
}

static const char *sdl_mode(void) { return s.mode; }

const CameraBackend camera_backend_sdl = { sdl_count, sdl_name, sdl_open, sdl_close, sdl_acquire, sdl_release, sdl_mode };
