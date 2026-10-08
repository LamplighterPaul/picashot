// Picashot: a photo booth. Launch it and the camera is on screen; Space takes a photo, V records a video.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "capture.h"
#include "filters.h"
#include "gpu.h"
#include "profile.h"

#ifndef PICASHOT_VERSION
#define PICASHOT_VERSION "dev"
#endif

#define GRID_COLS      4
#define COUNTDOWN_MS   3000
#define FLASH_MS       450
#define FRAME_MS       16       // redraw interval while something on screen is animating

static struct {
    SDL_Window  *window;
    int          camera_index;
    int          want_w, want_h;       // preferred camera size
    bool         force_sdl_camera;
    int          cam_w, cam_h;         // 0 until the first frame arrives
    Uint64       now;                  // time of the current loop turn; every timer below is measured from it
    Uint64       first_frame_ms;
    uint64_t     frame_stamp_ns, frame_arrived_ns;   // the frame now being drawn: taken, and received (CLOCK_MONOTONIC)

    int          filter;
    bool         mirror, timer, audio;
    bool         grid;
    int          hover;
    int          hover_button;         // on-screen button under the pointer: 0 none, 1 photo, 2 video, 3 timer, 4 sound
    SDL_Cursor  *hand;
    Uint64       countdown_end;        // 0 when no countdown is running
    Uint64       flash_start;
    bool         photo_pending;
    Uint64       record_start;
    bool         fullscreen;
    bool         running;
    bool         redraw;               // the window needs drawing again (interface changed)
    bool         refilter;             // the picture needs filtering again (filter or mirror changed)

    double       auto_snap, auto_record;   // seconds; < 0 when unused (--snap, --record)
    double       quit_after;               // seconds after the first camera frame; < 0 when unused
    const char  *shader_dir;               // hot reload: watch this folder for rebuilt .spv files
    Sint64       shader_times[64];
    Uint64       shader_checked;
} app = { .mirror = true, .timer = true, .audio = true, .hover = -1, .running = true, .redraw = true,
          .want_w = 1920, .want_h = 1080, .auto_snap = -1, .auto_record = -1, .quit_after = -1 };

static void update_title(void) {
    char title[160];
    const char *state = camera_count() == 0 ? "no camera found" : !camera_is_open() ? "camera in use by another app?"
                      : !app.cam_w ? "starting camera" : g_filters[app.filter].title;
    SDL_snprintf(title, sizeof title, "Picashot \xE2\x80\x94 %s", app.grid ? "choose a filter" : state);
    SDL_SetWindowTitle(app.window, title);
}

static void stop_recording(void) {
    if (!video_recording()) return;
    video_stop();
    app.record_start = 0;
    app.redraw = true;
}

static void close_camera(void) {
    stop_recording();
    camera_close();
    app.cam_w = app.cam_h = 0;
}

static void open_camera(int index) {
    close_camera();
    int n = camera_count();
    if (n > 0) {
        app.camera_index = ((index % n) + n) % n;
        if (!camera_open(app.camera_index, app.want_w, app.want_h, app.force_sdl_camera))
            fprintf(stderr, "picashot: could not open camera %d (is another app using it?)\n", app.camera_index);
        else if (SDL_getenv("PICASHOT_DEBUG") || profile_on)
            printf("camera: %s, %s (%s)\n", camera_name(app.camera_index), camera_mode(), camera_backend());
    }
    update_title();
}

// Hands the newest camera frame to the GPU. Returns true if there was one.
static bool pump_camera(void) {
    CamFrame frame;
    if (!camera_acquire(&frame)) return false;
    bool resized = frame.fmt.w != app.cam_w || frame.fmt.h != app.cam_h;
    if (resized) stop_recording();    // a recording cannot change size half way
    bool ok = gpu_upload(&frame);
    app.frame_stamp_ns = frame.stamp_ns;
    app.frame_arrived_ns = frame.arrived_ns;
    camera_release();
    if (ok && resized) {
        bool first = app.cam_w == 0;
        app.cam_w = frame.fmt.w;
        app.cam_h = frame.fmt.h;
        if (first) app.first_frame_ms = app.now;
        update_title();
    }
    return ok;
}

static void set_filter(int index) {
    app.filter = ((index % g_filter_count) + g_filter_count) % g_filter_count;
    app.refilter = true;
    update_title();
}

static void set_grid(bool on) {
    app.grid = on;
    app.hover = on ? app.filter : -1;
    app.refilter = true;
    update_title();
}

static void take_photo_now(void) {
    app.countdown_end = 0;
    app.photo_pending = true;
    app.flash_start = app.now;
}

static void shutter(void) {
    if (!app.cam_w) return;
    if (app.grid) set_grid(false);
    app.redraw = true;
    if (app.countdown_end) { app.countdown_end = 0; return; }   // pressing again cancels
    if (app.timer) app.countdown_end = app.now + COUNTDOWN_MS;
    else take_photo_now();
}

static void toggle_recording(void) {
    if (!app.cam_w) return;
    if (video_recording()) { stop_recording(); return; }
    if (app.grid) set_grid(false);
    if (video_start(gpu_video_width(), gpu_video_height(), app.audio)) app.record_start = app.now;
    app.redraw = true;
}

// Which grid tile is under a point given in window coordinates, or -1.
static int tile_at(float x, float y) {
    int w, h;
    SDL_GetWindowSize(app.window, &w, &h);
    if (w <= 0 || h <= 0 || !app.cam_w) return -1;
    float wa = (float)w / (float)h, ia = (float)app.cam_w / (float)app.cam_h;
    float sx = wa > ia ? ia / wa : 1.0f, sy = wa > ia ? 1.0f : wa / ia;
    float u = (x / (float)w - 0.5f) / sx + 0.5f, v = (y / (float)h - 0.5f) / sy + 0.5f;
    if (u < 0 || u >= 1 || v < 0 || v >= 1) return -1;
    int rows = (g_filter_count + GRID_COLS - 1) / GRID_COLS;
    int index = (int)(v * (float)rows) * GRID_COLS + (int)(u * (float)GRID_COLS);
    return index < g_filter_count ? index : -1;
}

// Which on-screen button is under a point in window coordinates: 0 none, 1 photo, 2 video, 3 timer,
// 4 sound. The layout matches shaders/present.frag.
static int button_at(float x, float y) {
    int w, h;
    SDL_GetWindowSize(app.window, &w, &h);
    if (w <= 0 || h <= 0 || app.grid) return 0;
    float unit = (float)SDL_min(w, h);
    float big = unit * 0.046f * 1.15f, little = unit * 0.024f * 1.35f;   // a little forgiving at the edge
    float mid = (float)w * 0.5f, dy = y - ((float)h - unit * 0.095f);
    const struct { float offset, reach; } buttons[4] = {
        { -unit * 0.075f, big }, { unit * 0.075f, big }, { -unit * 0.185f, little }, { unit * 0.185f, little } };
    for (int i = 0; i < 4; i++) {
        float dx = x - (mid + buttons[i].offset);
        if (dx * dx + dy * dy <= buttons[i].reach * buttons[i].reach) return i + 1;
    }
    return 0;
}

static void toggle_timer(void) { app.timer = !app.timer; app.redraw = true; }

// Sound cannot change half way through a recording: ffmpeg has already opened, or not opened, the microphone.
static void toggle_sound(void) {
    if (video_recording()) return;
    app.audio = !app.audio;
    app.redraw = true;
}

static void set_hover(int tile) {
    if (tile == app.hover) return;
    app.hover = tile;
    app.redraw = true;
}

static void set_hover_button(int button) {
    if (button == app.hover_button) return;
    app.hover_button = button;
    app.redraw = true;
    if (!app.hand) app.hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_SetCursor(button ? app.hand : SDL_GetDefaultCursor());
}

static void on_click(float x, float y, int clicks) {
    if (app.grid) {
        int t = tile_at(x, y);
        if (t >= 0) set_filter(t);
        set_grid(false);
        return;
    }
    int button = button_at(x, y);
    if (button == 1) shutter();
    else if (button == 2) toggle_recording();
    else if (button == 3) toggle_timer();
    else if (button == 4) toggle_sound();
    else if (clicks == 2 && !video_recording() && !app.countdown_end) set_grid(true);
}

static void on_key(SDL_Keycode key) {
    app.redraw = true;
    if (app.grid) {
        int h = app.hover < 0 ? app.filter : app.hover;
        switch (key) {
        case SDLK_LEFT:  app.hover = (h + g_filter_count - 1) % g_filter_count; return;
        case SDLK_RIGHT: app.hover = (h + 1) % g_filter_count; return;
        case SDLK_UP:    if (h - GRID_COLS >= 0) app.hover = h - GRID_COLS; return;
        case SDLK_DOWN:  if (h + GRID_COLS < g_filter_count) app.hover = h + GRID_COLS; return;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            set_filter(h);
            set_grid(false);
            return;
        case SDLK_ESCAPE: case SDLK_TAB: case SDLK_G: set_grid(false); return;
        default: break;
        }
    }
    switch (key) {
    case SDLK_SPACE: case SDLK_RETURN: case SDLK_KP_ENTER: shutter(); break;
    case SDLK_V: case SDLK_R: toggle_recording(); break;
    case SDLK_LEFT:  set_filter(app.filter - 1); break;
    case SDLK_RIGHT: set_filter(app.filter + 1); break;
    case SDLK_TAB: case SDLK_G:
        if (!video_recording() && !app.countdown_end) set_grid(true);
        break;
    case SDLK_M: app.mirror = !app.mirror; app.refilter = true; break;
    case SDLK_T: toggle_timer(); break;
    case SDLK_S: case SDLK_A: toggle_sound(); break;
    case SDLK_C: if (camera_count() > 1) open_camera(app.camera_index + 1); break;
    case SDLK_F: case SDLK_F11:
        app.fullscreen = !app.fullscreen;
        SDL_SetWindowFullscreen(app.window, app.fullscreen);
        break;
    case SDLK_ESCAPE:
        if (app.countdown_end) app.countdown_end = 0;
        else if (app.fullscreen) { app.fullscreen = false; SDL_SetWindowFullscreen(app.window, false); }
        else app.running = false;
        break;
    case SDLK_Q: app.running = false; break;
    default:
        if (key >= SDLK_1 && key <= SDLK_9 && key - SDLK_1 < (SDL_Keycode)g_filter_count) set_filter((int)(key - SDLK_1));
        break;
    }
}

static void on_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_QUIT: app.running = false; break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gpu_window_resized(); app.redraw = true; break;
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_WINDOW_RESTORED: app.redraw = true; break;
    case SDL_EVENT_KEY_DOWN: if (!e->key.repeat || e->key.key == SDLK_LEFT || e->key.key == SDLK_RIGHT) on_key(e->key.key); break;
    case SDL_EVENT_MOUSE_MOTION:
        if (app.grid) set_hover(tile_at(e->motion.x, e->motion.y));
        set_hover_button(button_at(e->motion.x, e->motion.y));
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: set_hover_button(0); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) on_click(e->button.x, e->button.y, e->button.clicks);
        break;
    case SDL_EVENT_MOUSE_WHEEL: if (!app.grid) set_filter(app.filter + (e->wheel.y < 0 ? 1 : -1)); break;
    case SDL_EVENT_CAMERA_DEVICE_DENIED:
        fprintf(stderr, "picashot: camera access was denied\n");
        close_camera();
        update_title();
        break;
    default: break;   // includes the camera's "frame ready" event, which only needs to wake the loop
    }
}

#ifdef PICASHOT_DEV
// Developer builds only (`make dev`): shader hot reload and scripted input. Neither exists in a release
// build, so nothing in the environment can drive the camera or load shader files there.

// Shader hot reload for filter authors: run with PICASHOT_SHADER_DIR=build/dev/spv, edit a shader,
// `make DEV=1 shaders`, and the running preview picks up the new version. The files are trusted
// developer output; the checks below only catch a half-written file.
static void reload_changed_shaders(Uint64 now) {
    if (!app.shader_dir || now - app.shader_checked < 300) return;
    bool first = app.shader_checked == 0;
    app.shader_checked = now;
    for (int i = 0; i < g_filter_count && i < 64; i++) {
        char path[1024];
        SDL_snprintf(path, sizeof path, "%s/%s.frag.spv", app.shader_dir, g_filters[i].name);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE || info.modify_time == app.shader_times[i]) continue;
        app.shader_times[i] = info.modify_time;
        if (first) continue;   // remember what is there at launch; the built-in copies are the same
        if (info.size < 20 || info.size > (1 << 20) || info.size % 4) { app.shader_times[i] = 0; continue; }
        size_t bytes = 0;
        uint32_t *code = SDL_LoadFile(path, &bytes);
        if (code && bytes >= 20 && bytes % 4 == 0 && code[0] == 0x07230203u && gpu_reload_filter(i, code, bytes)) {
            printf("reloaded %s\n", g_filters[i].name);
            app.refilter = true;
        } else {
            app.shader_times[i] = 0;   // probably half-written: try again next time round
        }
        SDL_free(code);
    }
}

// For automated tests: PICASHOT_KEYS="1.5:Tab 3:Space" presses those keys that many seconds after the
// first camera frame. Key names are SDL's; pointer actions are described below.
static void run_scripted_keys(Uint64 now) {
    static char script[512];
    static const char *next;
    static bool started;
    if (!started) {
        // "+" may be used instead of a space between entries, which is easier to pass through a shell.
        const char *env = SDL_getenv("PICASHOT_KEYS");
        if (env) { SDL_strlcpy(script, env, sizeof script); for (char *c = script; *c; c++) if (*c == '+') *c = ' '; next = script; }
        started = true;
    }
    while (next && *next) {
        char name[32];
        double at = 0;
        int used = 0;
        if (sscanf(next, " %lf:%31s%n", &at, name, &used) < 2) { next = NULL; break; }
        if ((double)(now - app.first_frame_ms) / 1000.0 < at) break;
        float fx, fy;
        int w, h;
        SDL_GetWindowSize(app.window, &w, &h);
        // "Click@0.5,0.9" clicks at that fraction of the window; "Hover@..." only moves the pointer there.
        if (sscanf(name, "Click@%f,%f", &fx, &fy) == 2) on_click(fx * (float)w, fy * (float)h, 1);
        else if (sscanf(name, "Hover@%f,%f", &fx, &fy) == 2) set_hover_button(button_at(fx * (float)w, fy * (float)h));
        else on_key(SDL_GetKeyFromName(name));
        next += used;
    }
}
static bool scripted_keys_waiting(void) { return SDL_getenv("PICASHOT_KEYS") != NULL; }
#else
static void reload_changed_shaders(Uint64 now) { (void)now; }
static void run_scripted_keys(Uint64 now) { (void)now; }
static bool scripted_keys_waiting(void) { return false; }
#endif

// Takes delivery of a finished capture: a photo goes to be saved, a video frame to the encoder.
static void deliver_capture(bool wait) {
    const uint8_t *photo, *video;
    if (!gpu_capture_pending() || !gpu_collect(wait, &photo, &video)) return;
    if (video && video_recording()) {
        uint64_t t0 = profile_now();
        video_frame(video);
        profile_since(PROF_VIDEO_COPY, t0);
    }
    if (photo) {
        photo_save(photo, gpu_camera_width(), gpu_camera_height());
        if (capture_quiet && !video_recording() && app.auto_record < 0) app.running = false;
    }
}

static void usage(void) {
    printf("Picashot %s: a photo booth for Linux\n\n"
           "  picashot [options]\n\n"
           "  --camera N        use camera number N (see --list-cameras)\n"
           "  --size WxH        preferred camera size (default 1920x1080)\n"
           "  --filter NAME     start with this filter (see --list-filters)\n"
           "  --no-mirror       do not mirror the picture\n"
           "  --no-timer        take photos at once, without the 3-second countdown\n"
           "  --no-audio        start with sound off for videos (the microphone button turns it back on)\n"
           "  --encoder NAME    video encoder: vaapi, nvenc, v4l2m2m, x264 or openh264 (default: best that works)\n"
           "  --sdl-camera      read the camera through SDL instead of directly (slower; for troubleshooting)\n"
           "  --snap SECONDS    take one photo after SECONDS and exit\n"
           "  --record SECONDS  record a video of SECONDS and exit\n"
           "  --quit-after SECONDS  exit that long after the camera starts (for benchmarks)\n"
           "  --list-cameras, --list-filters, --version, --help\n\n"
           "Keys: Space photo, V video, Left/Right filter, Tab filter grid, M mirror, T timer,\n"
           "      S sound on/off, C next camera, F fullscreen, Esc quit.\n", PICASHOT_VERSION);
}

// Parses a whole argument as a number within [lo, hi]. Rejects junk, infinities and NaN.
static bool parse_number(const char *s, double lo, double hi, double *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(v) || v < lo || v > hi) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv) {
    profile_init();
    double number;
    int camera_arg = 0;
    bool list_cameras = false;
    const char *encoder = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        char junk;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { printf("picashot %s\n", PICASHOT_VERSION); return 0; }
        else if (!strcmp(a, "--list-filters")) { for (int f = 0; f < g_filter_count; f++) printf("%-9s %s\n", g_filters[f].name, g_filters[f].title); return 0; }
        else if (!strcmp(a, "--list-cameras")) list_cameras = true;
        else if (!strcmp(a, "--no-mirror")) app.mirror = false;
        else if (!strcmp(a, "--no-timer")) app.timer = false;
        else if (!strcmp(a, "--no-audio")) app.audio = false;
        else if (!strcmp(a, "--sdl-camera")) app.force_sdl_camera = true;
        else if (!strcmp(a, "--encoder") && next) { encoder = next; i++; }
        else if (!strcmp(a, "--camera") && next && parse_number(next, 0, 63, &number)) { camera_arg = (int)number; i++; }
        else if (!strcmp(a, "--size") && next && sscanf(next, "%dx%d%c", &app.want_w, &app.want_h, &junk) == 2 &&
                 app.want_w >= 16 && app.want_h >= 16 && app.want_w <= GPU_MAX_CAMERA_SIDE && app.want_h <= GPU_MAX_CAMERA_SIDE) i++;
        else if (!strcmp(a, "--snap") && next && parse_number(next, 0, 3600, &app.auto_snap)) i++;
        else if (!strcmp(a, "--record") && next && parse_number(next, 0.1, 6 * 3600, &app.auto_record)) i++;
        else if (!strcmp(a, "--quit-after") && next && parse_number(next, 0, 24 * 3600, &app.quit_after)) i++;
        else if (!strcmp(a, "--filter") && next) {
            int found = -1;
            for (int f = 0; f < g_filter_count; f++) if (!SDL_strcasecmp(next, g_filters[f].name) || !SDL_strcasecmp(next, g_filters[f].title)) found = f;
            if (found < 0) { fprintf(stderr, "picashot: no filter called '%s' (see --list-filters)\n", next); return 2; }
            app.filter = found;
            i++;
        }
        else { fprintf(stderr, "picashot: bad option or value '%s' (see --help)\n", a); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);   // a dead ffmpeg must not take the app down with it
    capture_quiet = app.auto_snap >= 0 || app.auto_record >= 0;
#ifdef PICASHOT_DEV
    app.shader_dir = SDL_getenv("PICASHOT_SHADER_DIR");
#endif
    if (app.force_sdl_camera) SDL_setenv_unsafe("PICASHOT_CAMERA_SDL", "1", 1);

    SDL_SetAppMetadata("Picashot", PICASHOT_VERSION, "picashot");
    SDL_SetHint(SDL_HINT_APP_ID, "picashot");
    if (!SDL_Init(list_cameras ? SDL_INIT_CAMERA : SDL_INIT_VIDEO | SDL_INIT_CAMERA)) {
        fprintf(stderr, "picashot: %s\n", SDL_GetError());
        return 1;
    }
    if (list_cameras) {
        for (int i = 0; i < camera_count(); i++) printf("%d  %s\n", i, camera_name(i));
        if (camera_count() == 0) printf("no cameras found\n");
        SDL_Quit();
        return 0;
    }

    app.window = SDL_CreateWindow("Picashot", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) { fprintf(stderr, "picashot: could not open a window: %s\n", SDL_GetError()); return 1; }
    // Open the camera before the GPU is set up: the two start in parallel and the picture appears sooner.
    open_camera(camera_arg);
    CamFormat blank = { .w = 640, .h = 360, .layout = CAM_RGBA };
    if (!gpu_init(app.window) || !gpu_set_camera_format(&blank)) { gpu_shutdown(); SDL_Quit(); return 1; }
    if (camera_count() == 0) fprintf(stderr, "picashot: no camera found\n");
    if (!camera_is_open() && (app.auto_snap >= 0 || app.auto_record >= 0 || app.quit_after >= 0)) {
        // A scripted run has nobody to notice a black window: stop here.
        gpu_shutdown();
        SDL_Quit();
        return 1;
    }
    if (profile_on) printf("gpu: %s\n", gpu_device_name());

    Uint64 last_draw = 0;
    bool auto_started = false, encoder_started = false;
    while (app.running && gpu_ok()) {
        // Sleep until something happens: an input event, a camera frame (the v4l2 backend sends an
        // event for each), or the next moment something on screen has to move.
        Uint64 before = SDL_GetTicks();
        bool animating = app.countdown_end || (app.flash_start && before - app.flash_start < FLASH_MS);
        int wait_ms = 250;
        if (gpu_capture_pending()) wait_ms = 1;                                  // a capture is on its way back
        else if (app.redraw || app.refilter || app.photo_pending) wait_ms = 0;
        else if (animating) wait_ms = (int)SDL_max((Sint64)FRAME_MS - (Sint64)(before - last_draw), 0);
        else if (camera_is_open() && !camera_wake_event()) wait_ms = 4;          // the SDL backend has to be polled
        if (scripted_keys_waiting()) wait_ms = SDL_min(wait_ms, 20);
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            on_event(&e);
            while (SDL_PollEvent(&e)) on_event(&e);
        }
        Uint64 now = app.now = SDL_GetTicks();

        deliver_capture(false);
        uint64_t t0 = profile_now();
        bool new_frame = pump_camera();
        if (new_frame) { profile_since(PROF_CAMERA, t0); profile_camera_frame(); }
        profile_recording(video_recording(), video_encoder_pid(), video_dropped_frames());
        profile_report();
        reload_changed_shaders(now);
        run_scripted_keys(now);

        if (app.cam_w && !encoder_started) { video_prepare(encoder); encoder_started = true; }
        if (app.countdown_end && now >= app.countdown_end) take_photo_now();
        if (app.quit_after >= 0 && app.cam_w && (double)(now - app.first_frame_ms) / 1000.0 >= app.quit_after) app.running = false;

        // Scripted modes: wait for the camera to settle its exposure, do the job, leave.
        if (app.cam_w && (app.auto_snap >= 0 || app.auto_record >= 0)) {
            double since = (double)(now - app.first_frame_ms) / 1000.0;
            if (app.auto_snap >= 0 && since >= app.auto_snap) { take_photo_now(); app.flash_start = 0; app.auto_snap = -1; auto_started = true; }
            if (app.auto_record >= 0 && !auto_started && since >= 0.5) { toggle_recording(); auto_started = true; if (!video_recording()) app.running = false; }
            if (app.auto_record >= 0 && video_recording() && (double)(now - app.record_start) / 1000.0 >= app.auto_record) { toggle_recording(); app.running = false; }
        }

        // Draw when there is a new camera frame, when the interface changed, and at 60 Hz while the
        // countdown or the flash is moving. The filter runs again only when its input changed.
        float flash = app.flash_start && now - app.flash_start < FLASH_MS ? 1.0f - (float)(now - app.flash_start) / FLASH_MS : 0.0f;
        animating = app.countdown_end || flash > 0;
        bool want_photo = app.photo_pending && app.cam_w && !app.grid;
        if (!new_frame && !app.redraw && !app.refilter && !want_photo && !(animating && now - last_draw >= FRAME_MS)) continue;
        last_draw = now;

        deliver_capture(true);   // the previous capture has to be out of the way before the next frame
        int capture = (want_photo ? GPU_CAPTURE_PHOTO : 0) |
                      (video_recording() && new_frame && video_wants_frame() ? GPU_CAPTURE_VIDEO : 0);
        GpuFrame f = {
            .time = (float)now / 1000.0f, .filter = app.filter, .mirror = app.mirror,
            .grid_cols = app.grid ? GRID_COLS : 0, .hover = app.hover, .flash = flash * flash,
            .countdown = app.countdown_end ? (float)(app.countdown_end - now) / 1000.0f : 0.0f,
            .hover_button = app.hover_button, .sound_on = app.audio, .timer_on = app.timer,
            .rec_seconds = video_recording() ? (float)(now - app.record_start) / 1000.0f : -1.0f,
        };
        t0 = profile_now();
        bool drawn = gpu_frame(&f, app.refilter, capture);
        profile_since(capture ? PROF_DRAW_CAPTURE : PROF_DRAW, t0);
        app.redraw = app.refilter = false;
        if (drawn && want_photo) app.photo_pending = false;
        if (new_frame && app.cam_w) {
            uint64_t done = camera_monotonic_ns();
            if (app.frame_arrived_ns) profile_ms(PROF_LATENCY, (double)(done - app.frame_arrived_ns) / 1e6);
            if (app.frame_stamp_ns) profile_ms(PROF_LATENCY_SENSOR, (double)(done - app.frame_stamp_ns) / 1e6);
            profile_first_picture();
        }
    }

    deliver_capture(true);
    close_camera();
    capture_finish();
    if (app.hand) SDL_DestroyCursor(app.hand);
    gpu_shutdown();
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
