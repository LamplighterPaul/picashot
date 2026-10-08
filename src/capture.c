#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "capture.h"

extern char **environ;
bool capture_quiet;

static SDL_AtomicInt g_pending;   // photos and videos still being written
static SDL_AtomicInt g_photos;    // photo jobs running

#define MAX_PHOTO_JOBS 4

// Runs a program and waits for it, with stdin, stdout and stderr on /dev/null. Returns its exit code,
// or -1 if it could not be started or did not finish within `limit_ms` (it is killed then).
static int run_quiet(char *const argv[], int limit_ms) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) return -1;
    int status = 0;
    for (int waited = 0;; waited += 10) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (waited >= limit_ms) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return -1; }
        SDL_Delay(10);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void notify(const char *title, const char *body, const char *icon) {
    printf("%s: %s\n", title, body);
    fflush(stdout);
    if (capture_quiet) return;
    char *argv[] = { "notify-send", "-a", "Picashot", "-i", (char *)(icon ? icon : "camera-photo"),
                     (char *)title, (char *)body, NULL };
    run_quiet(argv, 3000);   // no notify-send installed is fine: the line above already went to the terminal
}

// Creates "<folder>/Picashot/Picashot 2026-10-08 at 14.03.22.<ext>" and returns it open for writing.
// The file is created exclusively (never an existing file, never through a symlink), so two photos in
// the same second, or two copies of the app, cannot overwrite each other; a counter is added instead.
static int output_create(char *path, size_t size, SDL_Folder folder, const char *ext) {
    const char *base = SDL_GetUserFolder(folder);
    if (!base) base = SDL_GetUserFolder(SDL_FOLDER_HOME);
    if (!base) return -1;
    char dir[900];
    if ((size_t)SDL_snprintf(dir, sizeof dir, "%sPicashot", base) >= sizeof dir) return -1;
    if (!SDL_CreateDirectory(dir)) return -1;
    SDL_Time now;
    SDL_DateTime dt;
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &dt, true);
    for (int n = 1; n < 100; n++) {
        char suffix[16] = "";
        if (n > 1) SDL_snprintf(suffix, sizeof suffix, " (%d)", n);
        int len = SDL_snprintf(path, size, "%s/Picashot %04d-%02d-%02d at %02d.%02d.%02d%s.%s", dir, dt.year, dt.month,
                               dt.day, dt.hour, dt.minute, dt.second, suffix, ext);
        if (len < 0 || (size_t)len >= size) return -1;
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);   // the umask decides who may read
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

// ---- photos ----

typedef struct {
    int      w, h, fd;
    uint8_t *rgba;
    char     path[1024];
} PhotoJob;

// An SDL stream that writes to a file descriptor we already hold.
static size_t SDLCALL fd_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status) {
    int fd = *(int *)userdata;
    size_t done = 0;
    while (done < size) {
        ssize_t k = write(fd, (const char *)ptr + done, size - done);
        if (k < 0) { if (errno == EINTR) continue; *status = SDL_IO_STATUS_ERROR; break; }
        done += (size_t)k;
    }
    return done;
}
static Sint64 SDLCALL fd_seek(void *userdata, Sint64 offset, SDL_IOWhence whence) {
    return (Sint64)lseek(*(int *)userdata, (off_t)offset, whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END);
}

static int SDLCALL photo_thread(void *arg) {
    PhotoJob *job = arg;
    bool ok = false;
    // The alpha channel is always opaque, so save three channels: the file is smaller.
    SDL_Surface *rgba = SDL_CreateSurfaceFrom(job->w, job->h, SDL_PIXELFORMAT_RGBA32, job->rgba, job->w * 4);
    SDL_Surface *rgb = rgba ? SDL_ConvertSurface(rgba, SDL_PIXELFORMAT_RGB24) : NULL;
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.write = fd_write;
    iface.seek = fd_seek;
    SDL_IOStream *io = rgb ? SDL_OpenIO(&iface, &job->fd) : NULL;
    if (io) ok = SDL_SavePNG_IO(rgb, io, true);
    SDL_DestroySurface(rgb);
    SDL_DestroySurface(rgba);
    if (close(job->fd) != 0) ok = false;
    if (ok) notify("Photo saved", job->path, job->path);
    else {
        fprintf(stderr, "picashot: could not save %s: %s\n", job->path, SDL_GetError());
        unlink(job->path);
        notify("Photo not saved", job->path, "dialog-error");
    }
    free(job->rgba);
    free(job);
    SDL_AddAtomicInt(&g_photos, -1);
    SDL_AddAtomicInt(&g_pending, -1);
    return 0;
}

void photo_save(const uint8_t *rgba, int w, int h) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384) return;
    if (SDL_GetAtomicInt(&g_photos) >= MAX_PHOTO_JOBS) {
        fprintf(stderr, "picashot: still saving earlier photos, this one was skipped\n");
        return;
    }
    PhotoJob *job = calloc(1, sizeof *job);
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (job) job->fd = -1;
    if (!job || !(job->rgba = malloc(bytes)) || (job->fd = output_create(job->path, sizeof job->path, SDL_FOLDER_PICTURES, "png")) < 0) {
        fprintf(stderr, "picashot: could not prepare the photo file\n");
        if (job) free(job->rgba);
        free(job);
        return;
    }
    memcpy(job->rgba, rgba, bytes);
    job->w = w;
    job->h = h;
    SDL_AddAtomicInt(&g_pending, 1);
    SDL_AddAtomicInt(&g_photos, 1);
    SDL_Thread *t = SDL_CreateThread(photo_thread, "picashot-photo", job);
    if (t) SDL_DetachThread(t);
    else photo_thread(job);
}

// ---- choosing a video encoder ----

// Each candidate is a list of ffmpeg arguments placed after the inputs. "@" is replaced by a DRM
// render node for the encoders that need one. Tried top to bottom; the first that encodes a short
// test clip wins. GPU encoders come first: on the machine this was developed on they use about an
// eighth of the CPU of the software one.
typedef struct {
    const char *name, *short_name;
    const char *global[4];    // arguments placed before the inputs
    const char *output[12];
} Encoder;

static const Encoder encoders[] = {
    { "h264_vaapi", "vaapi", { "-vaapi_device", "@" }, { "-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-qp", "22" } },
    { "h264_nvenc", "nvenc", { 0 }, { "-c:v", "h264_nvenc", "-preset", "p4", "-rc", "constqp", "-qp", "23" } },
    { "h264_v4l2m2m", "v4l2m2m", { 0 }, { "-c:v", "h264_v4l2m2m", "-b:v", "8M" } },
    { "libx264", "x264", { 0 }, { "-c:v", "libx264", "-preset", "superfast", "-crf", "21", "-pix_fmt", "yuv420p" } },
    { "libopenh264", "openh264", { 0 }, { "-c:v", "libopenh264", "-b:v", "8M", "-pix_fmt", "yuv420p" } },
};

static struct {
    SDL_AtomicInt state;        // 0 not started, 1 probing, 2 done
    SDL_Thread   *thread;
    const Encoder *chosen;
    char          render_node[64];
    char          forced[32];
} enc;

static int add_args(char **argv, int n, const char *const *list, size_t max, const char *node) {
    for (size_t i = 0; i < max && list[i]; i++) argv[n++] = (char *)(list[i][0] == '@' ? node : list[i]);
    return n;
}

static bool encoder_works(const Encoder *e, const char *node) {
    char *argv[40];
    int n = 0;
    const char *head[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin" };
    const char *input[] = { "-f", "lavfi", "-i", "color=black:s=640x360:r=30:d=0.2,format=nv12" };
    const char *tail[] = { "-f", "null", "-" };
    n = add_args(argv, n, head, SDL_arraysize(head), node);
    n = add_args(argv, n, e->global, SDL_arraysize(e->global), node);
    n = add_args(argv, n, input, SDL_arraysize(input), node);
    n = add_args(argv, n, e->output, SDL_arraysize(e->output), node);
    n = add_args(argv, n, tail, SDL_arraysize(tail), node);
    argv[n] = NULL;
    return run_quiet(argv, 5000) == 0;
}

static int SDLCALL probe_thread(void *arg) {
    (void)arg;
    const Encoder *fallback = NULL;
    for (size_t i = 0; i < SDL_arraysize(encoders) && !enc.chosen; i++) {
        const Encoder *e = &encoders[i];
        if (!SDL_strcmp(e->name, "libx264")) fallback = e;
        if (enc.forced[0] && SDL_strcasecmp(enc.forced, e->short_name) && SDL_strcasecmp(enc.forced, e->name)) continue;
        if (e->global[1] && e->global[1][0] == '@') {
            // Needs a render node: try each GPU in the machine.
            for (int node = 128; node < 136 && !enc.chosen; node++) {
                SDL_snprintf(enc.render_node, sizeof enc.render_node, "/dev/dri/renderD%d", node);
                if (access(enc.render_node, R_OK | W_OK) == 0 && encoder_works(e, enc.render_node)) enc.chosen = e;
            }
        } else if (encoder_works(e, "")) {
            enc.chosen = e;
        }
    }
    if (!enc.chosen) enc.chosen = fallback;   // let ffmpeg report the real problem when a recording starts
    SDL_SetAtomicInt(&enc.state, 2);
    return 0;
}

void video_prepare(const char *forced) {
    if (!SDL_CompareAndSwapAtomicInt(&enc.state, 0, 1)) return;
    if (forced) SDL_strlcpy(enc.forced, forced, sizeof enc.forced);
    enc.thread = SDL_CreateThread(probe_thread, "picashot-probe", NULL);
    if (!enc.thread) probe_thread(NULL);
}

static const Encoder *encoder_ready(void) {
    video_prepare(NULL);
    if (enc.thread) { SDL_WaitThread(enc.thread, NULL); enc.thread = NULL; }
    return enc.chosen;
}

const char *video_encoder_name(void) {
    return SDL_GetAtomicInt(&enc.state) == 2 && enc.chosen ? enc.chosen->name : "";
}

// ---- video ----

#define QUEUE          12       // about 0.4 s of frames: covers the moment ffmpeg and a GPU encoder take to start
#define DRAIN_LIMIT_MS 3000     // after Stop: how long queued frames may take to reach ffmpeg
#define EXIT_LIMIT_MS  5000     // then how long ffmpeg may take to finish the file before it is told to

static struct {
    bool           active;       // recording; main thread only
    bool           finishing;    // stopped, ffmpeg still writing the file; main thread only
    bool           with_audio;
    size_t         frame_bytes;
    pid_t          ffmpeg;
    int            pipe_fd;
    char           path[1024];
    SDL_Thread    *thread;
    SDL_Mutex     *lock;
    SDL_Condition *wake;
    uint8_t       *slots[QUEUE];
    int            head, count;  // ring of filled slots, guarded by lock
    bool           stopping, failed;
    Uint64         stop_at;      // SDL ticks when Stop was pressed
    SDL_AtomicInt  dropped;
} v;

// Writes a whole frame to the (non-blocking) pipe. Gives up if ffmpeg has gone, or if Stop was pressed
// and the drain limit has passed, so a stuck ffmpeg can never hold the microphone open for long.
static bool write_frame(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k > 0) { p += k; n -= (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno != EAGAIN) return false;
        struct pollfd pfd = { fd, POLLOUT, 0 };
        poll(&pfd, 1, 100);
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        SDL_LockMutex(v.lock);
        bool give_up = v.stopping && SDL_GetTicks() - v.stop_at > DRAIN_LIMIT_MS;
        SDL_UnlockMutex(v.lock);
        if (give_up) return false;
    }
    return true;
}

// Waits for ffmpeg to exit. It normally finishes by itself within a second of its input closing; if it
// does not, it is asked (it writes a complete file on SIGINT) and finally killed. Returns its wait status.
static int reap_ffmpeg(void) {
    int status = 0;
    Uint64 start = SDL_GetTicks();
    bool interrupted = false, killed = false;
    for (;;) {
        pid_t r = waitpid(v.ffmpeg, &status, WNOHANG);
        if (r == v.ffmpeg || (r < 0 && errno != EINTR)) return status;
        Uint64 waited = SDL_GetTicks() - start;
        if (!interrupted && waited > EXIT_LIMIT_MS) { kill(v.ffmpeg, SIGINT); interrupted = true; }
        if (!killed && waited > EXIT_LIMIT_MS + 3000) { kill(v.ffmpeg, SIGKILL); killed = true; }
        SDL_Delay(20);
    }
}

// Feeds frames to ffmpeg, then closes it down and reports the result.
static int SDLCALL video_thread(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(v.lock);
        while (v.count == 0 && !v.stopping) SDL_WaitCondition(v.wake, v.lock);
        if (v.count == 0) { SDL_UnlockMutex(v.lock); break; }
        uint8_t *frame = v.slots[v.head];
        bool skip = v.failed;
        SDL_UnlockMutex(v.lock);

        bool ok = skip || write_frame(v.pipe_fd, frame, v.frame_bytes);

        SDL_LockMutex(v.lock);
        if (!ok) v.failed = true;   // ffmpeg went away or is stuck; keep emptying the queue so nothing blocks
        v.head = (v.head + 1) % QUEUE;
        v.count--;
        SDL_UnlockMutex(v.lock);
    }
    close(v.pipe_fd);
    int status = reap_ffmpeg();
    // 255 is what ffmpeg returns after a SIGINT that it handled cleanly.
    bool ok = !v.failed && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 255);
    int dropped = SDL_GetAtomicInt(&v.dropped);
    if (dropped) fprintf(stderr, "picashot: the encoder fell behind, %d frame(s) dropped\n", dropped);
    struct stat st;
    if (ok && stat(v.path, &st) == 0 && st.st_size > 0) notify("Video saved", v.path, "camera-video");
    else {
        unlink(v.path);
        notify("Video not saved", v.with_audio ? "ffmpeg failed. Try again with --no-audio." : "ffmpeg failed.", "dialog-error");
    }
    SDL_AddAtomicInt(&g_pending, -1);
    return 0;
}

// Waits for a stopped recording to finish writing and frees its memory.
static void video_reap(void) {
    if (!v.finishing) return;
    SDL_WaitThread(v.thread, NULL);
    for (int i = 0; i < QUEUE; i++) { free(v.slots[i]); v.slots[i] = NULL; }
    v.finishing = false;
}

bool video_start(int w, int h, bool with_audio) {
    if (v.active) return true;
    if (w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
    video_reap();
    const Encoder *e = encoder_ready();
    if (!e) { notify("Video needs ffmpeg", "Install ffmpeg to record videos.", "dialog-error"); return false; }
    if (!v.lock) { v.lock = SDL_CreateMutex(); v.wake = SDL_CreateCondition(); }
    if (!v.lock || !v.wake) return false;

    int out_fd = -1, fds[2] = { -1, -1 };
    v.frame_bytes = (size_t)w * (size_t)h * 3 / 2;
    for (int i = 0; i < QUEUE; i++) v.slots[i] = malloc(v.frame_bytes);
    bool ok = true;
    for (int i = 0; i < QUEUE; i++) ok = ok && v.slots[i];
    ok = ok && (out_fd = output_create(v.path, sizeof v.path, SDL_FOLDER_VIDEOS, "mp4")) >= 0;
    ok = ok && pipe2(fds, O_CLOEXEC) == 0;
    if (!ok) goto fail;
    fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);                     // fewer, larger writes per frame; best effort
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);

    // Build the whole command before forking: after fork only simple system calls are safe.
    // Frames carry the time they arrive at, so the video keeps real time even if a frame is dropped,
    // and stays in step with the microphone. The picture is BT.709 limited range; the input is tagged
    // so, which the encoder carries into the file.
    char size[32];
    SDL_snprintf(size, sizeof size, "%dx%d", w, h);
    char *argv[80];
    int n = 0;
    const char *head[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y" };
    const char *video_in[] = { "-f", "rawvideo", "-pixel_format", "nv12", "-video_size", size, "-framerate", "30",
        "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv",
        "-use_wallclock_as_timestamps", "1", "-thread_queue_size", "8", "-i", "pipe:0" };
    const char *audio_in[] = { "-f", "pulse", "-thread_queue_size", "1024", "-i", "default" };
    // The microphone never ends by itself; "-shortest" ends the file when the video does. That is how a
    // recording stops (we close the pipe), and it also means that if Picashot dies for any reason the
    // pipe closes, ffmpeg finishes a playable file and lets go of the microphone.
    const char *audio_out[] = { "-c:a", "aac", "-b:a", "160k", "-shortest" };
    const char *tail[] = { "-r", "30", "-f", "mp4", "/dev/fd/3" };
    n = add_args(argv, n, head, SDL_arraysize(head), enc.render_node);
    n = add_args(argv, n, e->global, SDL_arraysize(e->global), enc.render_node);
    n = add_args(argv, n, video_in, SDL_arraysize(video_in), enc.render_node);
    if (with_audio) n = add_args(argv, n, audio_in, SDL_arraysize(audio_in), enc.render_node);
    n = add_args(argv, n, e->output, SDL_arraysize(e->output), enc.render_node);
    if (with_audio) n = add_args(argv, n, audio_out, SDL_arraysize(audio_out), enc.render_node);
    n = add_args(argv, n, tail, SDL_arraysize(tail), enc.render_node);
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        // In ffmpeg-to-be. Frames arrive on stdin and the video file is descriptor 3. dup2 clears close-on-exec on the
        // copies; every other descriptor we opened closes itself at exec. Move the sources out of the
        // way first in case one of them already sits on 0 or 3.
        int in = fcntl(fds[0], F_DUPFD_CLOEXEC, 10), out = fcntl(out_fd, F_DUPFD_CLOEXEC, 10);
        if (in < 0 || out < 0 || dup2(in, 0) < 0 || dup2(out, 3) < 0) _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(fds[0]);
    close(out_fd);
    fds[0] = out_fd = -1;

    v.ffmpeg = pid;
    v.pipe_fd = fds[1];
    v.with_audio = with_audio;
    v.head = v.count = 0;
    v.stopping = v.failed = false;
    SDL_SetAtomicInt(&v.dropped, 0);
    SDL_AddAtomicInt(&g_pending, 1);
    v.thread = SDL_CreateThread(video_thread, "picashot-video", NULL);
    if (!v.thread) {
        // Nobody will feed or reap ffmpeg: do it here. Closing the pipe ends its input.
        SDL_AddAtomicInt(&g_pending, -1);
        close(fds[1]);
        fds[1] = -1;
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        unlink(v.path);
        goto fail;
    }
    v.active = true;
    return true;

fail:
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
    if (out_fd >= 0) { close(out_fd); unlink(v.path); }
    for (int i = 0; i < QUEUE; i++) { free(v.slots[i]); v.slots[i] = NULL; }
    notify("Video not started", "Could not start recording.", "dialog-error");
    return false;
}

bool video_wants_frame(void) {
    if (!v.active) return false;
    SDL_LockMutex(v.lock);
    bool room = v.count < QUEUE;
    SDL_UnlockMutex(v.lock);
    if (!room) SDL_AddAtomicInt(&v.dropped, 1);
    return room;
}

void video_frame(const uint8_t *nv12) {
    if (!v.active) return;
    SDL_LockMutex(v.lock);
    if (v.count == QUEUE) { SDL_AddAtomicInt(&v.dropped, 1); SDL_UnlockMutex(v.lock); return; }
    uint8_t *slot = v.slots[(v.head + v.count) % QUEUE];
    SDL_UnlockMutex(v.lock);
    memcpy(slot, nv12, v.frame_bytes);   // only this thread adds, so the slot stays free while we fill it
    SDL_LockMutex(v.lock);
    v.count++;
    SDL_SignalCondition(v.wake);
    SDL_UnlockMutex(v.lock);
}

// Stopping does not wait for ffmpeg: the thread finishes the file by itself and the preview carries on.
void video_stop(void) {
    if (!v.active) return;
    SDL_LockMutex(v.lock);
    v.stopping = true;
    v.stop_at = SDL_GetTicks();
    SDL_SignalCondition(v.wake);
    SDL_UnlockMutex(v.lock);
    v.active = false;
    v.finishing = true;
}

bool video_recording(void) { return v.active; }
int  video_encoder_pid(void) { return v.active ? (int)v.ffmpeg : 0; }
int  video_dropped_frames(void) { return SDL_GetAtomicInt(&v.dropped); }

void capture_finish(void) {
    video_stop();
    video_reap();
    if (enc.thread) { SDL_WaitThread(enc.thread, NULL); enc.thread = NULL; }
    while (SDL_GetAtomicInt(&g_pending) > 0) SDL_Delay(10);
}
