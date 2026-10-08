#define _POSIX_C_SOURCE 200809L
#include <SDL3/SDL.h>
#include <time.h>

#include "camera.h"

uint32_t camera_event_type;
static const CameraBackend *active, *lister;
static int offset_v4l2;   // how many cameras the list takes from the v4l2 backend

uint64_t camera_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

long camera_score_mode(int w, int h, double fps, int want_w, int want_h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    long pixels = (long)w * h;
    // Up to the wanted size, bigger wins. Above it is a last resort, smallest first.
    long score = (w <= want_w && h <= want_h) ? 200000000L + pixels : 100000000L - pixels;
    // 30 fps is the target: a 60 fps mode would double the work for a 30 fps video, and anything
    // slower than 24 looks choppy enough that a smaller picture is the better trade.
    if (fps >= 24.0 && fps <= 35.0) score += 400000000L;
    else if (fps > 35.0) score += 300000000L;
    return score;
}

// The list is the v4l2 backend's cameras. If it finds none (no /dev/video*, or a sandbox), it is SDL's.
static void pick_lister(void) {
    if (lister) return;
    if (!camera_event_type) camera_event_type = SDL_RegisterEvents(1);
    offset_v4l2 = SDL_getenv("PICASHOT_CAMERA_SDL") ? 0 : camera_backend_v4l2.count();
    lister = offset_v4l2 > 0 ? &camera_backend_v4l2 : &camera_backend_sdl;
}

int camera_count(void) { pick_lister(); return lister->count(); }
const char *camera_name(int index) { pick_lister(); return lister->name(index); }

bool camera_open(int index, int want_w, int want_h, bool force_sdl) {
    pick_lister();
    camera_close();
    if (lister == &camera_backend_v4l2 && !force_sdl && camera_backend_v4l2.open(index, want_w, want_h)) {
        active = &camera_backend_v4l2;
        return true;
    }
    // SDL numbers cameras its own way; the same index is the best guess we have.
    int n = camera_backend_sdl.count();
    if (n > 0 && camera_backend_sdl.open(index < n ? index : 0, want_w, want_h)) {
        active = &camera_backend_sdl;
        return true;
    }
    return false;
}

void camera_close(void) {
    if (active) active->close();
    active = NULL;
}

bool camera_is_open(void) { return active != NULL; }
const char *camera_backend(void) { return active == &camera_backend_v4l2 ? "v4l2" : active ? "sdl" : "none"; }
const char *camera_mode(void) { return active ? active->mode() : ""; }
bool camera_acquire(CamFrame *out) { return active && active->acquire(out); }
void camera_release(void) { if (active) active->release(); }
uint32_t camera_wake_event(void) { return active == &camera_backend_v4l2 ? camera_event_type : 0; }
