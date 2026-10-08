// Our own camera backend: Video4Linux2 with memory-mapped buffers.
//
// A capture thread waits for the driver, takes each frame and turns it into plain YUV planes in one of
// three frame stores: raw NV12 and YUYV frames are copied, MJPEG frames are decoded by libjpeg-turbo
// straight to planes (no RGB step; the GPU does the colour conversion). The newest finished store is
// handed to the main thread, older unseen ones are simply overwritten, so a slow main loop sees the
// latest picture and never a queue of stale ones.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#include "camera.h"

#define MAX_DEVICES 16
#define MAX_BUFFERS 4
#define MAX_SIDE    8192

typedef struct {
    char path[32];
    char name[64];
} Device;

// One decoded frame. `data` holds the planes back to back.
typedef struct {
    uint8_t  *data;
    size_t    size;
    CamFormat fmt;
    uint8_t  *plane[3];
    int       pitch[3];
    uint64_t  stamp_ns, arrived_ns;
} Store;

static struct {
    Device   devices[MAX_DEVICES];
    int      device_count;
    bool     listed;

    int      fd;
    int      wake[2];                  // pipe: closing the write end stops the thread
    uint32_t pixfmt;
    int      w, h;
    bool     full_range, bt709;
    struct { void *start; size_t length; } buffers[MAX_BUFFERS];
    int      buffer_count;
    char     mode[96];

    SDL_Thread *thread;
    SDL_Mutex  *lock;
    Store    stores[3];
    int      writing, ready, reading;  // indices into stores; ready and reading are -1 when empty
    bool     fresh;                    // `ready` has not been seen by the main thread yet
#ifdef HAVE_TURBOJPEG
    tjhandle jpeg;
#endif
} c = { .fd = -1, .wake = { -1, -1 }, .ready = -1, .reading = -1 };

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg); while (r < 0 && errno == EINTR);
    return r;
}

static bool format_supported(uint32_t pixfmt) {
    if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_YUYV) return true;
#ifdef HAVE_TURBOJPEG
    if (pixfmt == V4L2_PIX_FMT_MJPEG || pixfmt == V4L2_PIX_FMT_JPEG) return true;
#endif
    return false;
}

static bool is_jpeg(uint32_t pixfmt) { return pixfmt == V4L2_PIX_FMT_MJPEG || pixfmt == V4L2_PIX_FMT_JPEG; }

// A capture device we can use: it streams video and offers at least one format we understand.
// (A webcam usually shows up as two nodes; the second carries metadata and offers no formats.)
static bool usable(int fd, char *name, size_t name_size) {
    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return false;
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) return false;
    for (uint32_t i = 0; i < 64; i++) {
        struct v4l2_fmtdesc desc = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
        if (format_supported(desc.pixelformat)) {
            SDL_strlcpy(name, (const char *)cap.card, name_size);
            return true;
        }
    }
    return false;
}

static int v4l2_count(void) {
    if (c.listed) return c.device_count;
    c.listed = true;
    for (int i = 0; i < 64 && c.device_count < MAX_DEVICES; i++) {
        Device *d = &c.devices[c.device_count];
        SDL_snprintf(d->path, sizeof d->path, "/dev/video%d", i);
        int fd = open(d->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (usable(fd, d->name, sizeof d->name)) c.device_count++;
        close(fd);
    }
    return c.device_count;
}

static const char *v4l2_name(int index) {
    return index >= 0 && index < v4l2_count() ? c.devices[index].name : "camera";
}

// Looks through every format, size and rate the device offers and keeps the best by camera_score_mode.
// At equal score an uncompressed format wins: it needs no decoding.
static bool choose_mode(int fd, int want_w, int want_h, uint32_t *pixfmt, int *w, int *h, struct v4l2_fract *interval) {
    long best = -1;
    for (uint32_t f = 0; f < 64; f++) {
        struct v4l2_fmtdesc desc = { .index = f, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
        if (!format_supported(desc.pixelformat)) continue;
        for (uint32_t s = 0; s < 256; s++) {
            struct v4l2_frmsizeenum size = { .index = s, .pixel_format = desc.pixelformat };
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
            int sw, sh;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) { sw = (int)size.discrete.width; sh = (int)size.discrete.height; }
            else {   // a range: take the wanted size, clamped into it
                sw = (int)SDL_clamp((uint32_t)want_w, size.stepwise.min_width, size.stepwise.max_width);
                sh = (int)SDL_clamp((uint32_t)want_h, size.stepwise.min_height, size.stepwise.max_height);
            }
            for (uint32_t r = 0; r < 64; r++) {
                struct v4l2_frmivalenum ival = { .index = r, .pixel_format = desc.pixelformat,
                                                 .width = (uint32_t)sw, .height = (uint32_t)sh };
                struct v4l2_fract fr = { 1, 30 };
                bool listed = xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival) == 0;
                if (!listed && r > 0) break;
                if (listed && ival.type == V4L2_FRMIVAL_TYPE_DISCRETE) fr = ival.discrete;
                else if (listed) fr = ival.stepwise.min;   // a range: its fastest end, capped below by the score
                double fps = fr.numerator ? (double)fr.denominator / fr.numerator : 0;
                long score = camera_score_mode(sw, sh, fps, want_w, want_h) * 2 + (is_jpeg(desc.pixelformat) ? 0 : 1);
                if (score > best) { best = score; *pixfmt = desc.pixelformat; *w = sw; *h = sh; *interval = fr; }
                if (!listed || ival.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
            }
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
        }
    }
    return best >= 0;
}

static void stores_free(void) {
    for (int i = 0; i < 3; i++) { free(c.stores[i].data); c.stores[i] = (Store){ 0 }; }
}

// Makes sure a store can hold a frame of this format and points its planes into the memory.
static bool store_shape(Store *s, const CamFormat *fmt) {
    size_t y = (size_t)fmt->w * (size_t)fmt->h, need;
    switch (fmt->layout) {
    case CAM_NV12:   need = y + (size_t)fmt->chroma_w * 2 * (size_t)fmt->chroma_h; break;
    case CAM_YUYV:   need = y * 2; break;
    case CAM_PLANAR: need = y + (size_t)fmt->chroma_w * (size_t)fmt->chroma_h * 2; break;
    default:         return false;
    }
    if (need > s->size) {
        uint8_t *grown = realloc(s->data, need);
        if (!grown) return false;
        s->data = grown;
        s->size = need;
    }
    s->fmt = *fmt;
    s->plane[0] = s->data;
    s->pitch[0] = fmt->layout == CAM_YUYV ? fmt->w * 2 : fmt->w;
    s->plane[1] = s->plane[2] = NULL;
    s->pitch[1] = s->pitch[2] = 0;
    if (fmt->layout == CAM_NV12) { s->plane[1] = s->data + y; s->pitch[1] = fmt->chroma_w * 2; }
    if (fmt->layout == CAM_PLANAR) {
        s->plane[1] = s->data + y;
        s->plane[2] = s->plane[1] + (size_t)fmt->chroma_w * (size_t)fmt->chroma_h;
        s->pitch[1] = s->pitch[2] = fmt->chroma_w;
    }
    return true;
}

// Turns one driver buffer into planes in `s`. Returns false for a frame we cannot use (a damaged JPEG,
// a short buffer): the caller drops it and waits for the next.
static bool fill_store(Store *s, const uint8_t *src, size_t bytes) {
    CamFormat fmt = { .w = c.w, .h = c.h, .full_range = c.full_range, .bt709 = c.bt709 };
    if (c.pixfmt == V4L2_PIX_FMT_NV12) {
        fmt.layout = CAM_NV12;
        fmt.chroma_w = c.w / 2;
        fmt.chroma_h = c.h / 2;
        size_t need = (size_t)c.w * (size_t)c.h + (size_t)fmt.chroma_w * 2 * (size_t)fmt.chroma_h;
        if (bytes < need || !store_shape(s, &fmt)) return false;
        memcpy(s->data, src, need);
        return true;
    }
    if (c.pixfmt == V4L2_PIX_FMT_YUYV) {
        fmt.layout = CAM_YUYV;
        size_t need = (size_t)c.w * (size_t)c.h * 2;
        if (bytes < need || !store_shape(s, &fmt)) return false;
        memcpy(s->data, src, need);
        return true;
    }
#ifdef HAVE_TURBOJPEG
    if (is_jpeg(c.pixfmt)) {
        if (bytes < 4 || tj3DecompressHeader(c.jpeg, src, bytes) < 0) return false;
        int jw = tj3Get(c.jpeg, TJPARAM_JPEGWIDTH), jh = tj3Get(c.jpeg, TJPARAM_JPEGHEIGHT);
        int sub = tj3Get(c.jpeg, TJPARAM_SUBSAMP);
        // The picture must be the size we set the camera to, and in a YUV layout we can show.
        if (jw != c.w || jh != c.h || sub < 0 || sub == TJSAMP_GRAY || sub == TJSAMP_UNKNOWN) return false;
        fmt.layout = CAM_PLANAR;
        fmt.full_range = true;    // JPEG is always full-range BT.601
        fmt.bt709 = false;
        fmt.chroma_w = tj3YUVPlaneWidth(1, jw, sub);
        fmt.chroma_h = tj3YUVPlaneHeight(1, jh, sub);
        if (fmt.chroma_w <= 0 || fmt.chroma_h <= 0 || !store_shape(s, &fmt)) return false;
        return tj3DecompressToYUVPlanes8(c.jpeg, src, bytes, s->plane, s->pitch) == 0;
    }
#endif
    return false;
}

static int SDLCALL capture_thread(void *arg) {
    (void)arg;
    struct pollfd fds[2] = { { c.fd, POLLIN, 0 }, { c.wake[0], POLLIN, 0 } };
    for (;;) {
        int r = poll(fds, 2, 2000);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) break;   // asked to stop
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;            // the camera went away
        if (!(fds[0].revents & POLLIN)) continue;

        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(c.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }
        bool ok = false;
        uint64_t arrived = camera_monotonic_ns();
        if (buf.index < (uint32_t)c.buffer_count && !(buf.flags & V4L2_BUF_FLAG_ERROR) &&
            buf.bytesused <= c.buffers[buf.index].length) {
            Store *s = &c.stores[c.writing];
            ok = fill_store(s, c.buffers[buf.index].start, buf.bytesused);
            // The driver stamps frames on CLOCK_MONOTONIC unless it says otherwise.
            bool monotonic = (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            s->arrived_ns = arrived;
            s->stamp_ns = monotonic ? (uint64_t)buf.timestamp.tv_sec * 1000000000ull + (uint64_t)buf.timestamp.tv_usec * 1000ull
                                    : arrived;
        }
        xioctl(c.fd, VIDIOC_QBUF, &buf);
        if (!ok) continue;

        // Publish: the store just written becomes `ready`; write next into whichever store is free.
        SDL_LockMutex(c.lock);
        int was_ready = c.ready;
        c.ready = c.writing;
        c.fresh = true;
        if (was_ready >= 0) c.writing = was_ready;
        else for (int i = 0; i < 3; i++) if (i != c.ready && i != c.reading) { c.writing = i; break; }
        SDL_UnlockMutex(c.lock);

        SDL_Event wake = { .type = camera_event_type };
        SDL_PushEvent(&wake);
    }
    return 0;
}

static void v4l2_close(void) {
    if (c.thread) {
        if (c.wake[1] >= 0) { close(c.wake[1]); c.wake[1] = -1; }
        SDL_WaitThread(c.thread, NULL);
        c.thread = NULL;
    }
    if (c.wake[0] >= 0) { close(c.wake[0]); c.wake[0] = -1; }
    if (c.wake[1] >= 0) { close(c.wake[1]); c.wake[1] = -1; }
    if (c.fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(c.fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < c.buffer_count; i++)
            if (c.buffers[i].start) munmap(c.buffers[i].start, c.buffers[i].length);
        close(c.fd);
        c.fd = -1;
    }
    memset(c.buffers, 0, sizeof c.buffers);
    c.buffer_count = 0;
#ifdef HAVE_TURBOJPEG
    if (c.jpeg) { tj3Destroy(c.jpeg); c.jpeg = NULL; }
#endif
    stores_free();
    c.writing = 0;
    c.ready = c.reading = -1;
    c.fresh = false;
}

static bool fail(const char *what) {
    fprintf(stderr, "picashot: camera: %s: %s\n", what, strerror(errno));
    v4l2_close();
    return false;
}

static bool v4l2_open(int index, int want_w, int want_h) {
    if (index < 0 || index >= v4l2_count()) return false;
    c.fd = open(c.devices[index].path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c.fd < 0) return fail(c.devices[index].path);

    uint32_t pixfmt = 0;
    int w = 0, h = 0;
    struct v4l2_fract interval = { 1, 30 };
    if (!choose_mode(c.fd, want_w, want_h, &pixfmt, &w, &h, &interval)) { errno = ENOTSUP; return fail("no usable mode"); }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = (uint32_t)w;
    fmt.fmt.pix.height = (uint32_t)h;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(c.fd, VIDIOC_S_FMT, &fmt) < 0) return fail("setting the mode (is another app using the camera?)");
    // The driver may adjust what we asked for; what it reports back is what we get.
    c.pixfmt = fmt.fmt.pix.pixelformat;
    c.w = (int)fmt.fmt.pix.width;
    c.h = (int)fmt.fmt.pix.height;
    if (!format_supported(c.pixfmt) || c.w < 2 || c.h < 2 || c.w > MAX_SIDE || c.h > MAX_SIDE || (c.w & 1) || (c.h & 1)) {
        errno = ENOTSUP;
        return fail("the camera chose a mode we cannot use");
    }
    // Colour: use what the driver says, with the standard defaults where it says nothing.
    uint32_t enc = fmt.fmt.pix.ycbcr_enc, quant = fmt.fmt.pix.quantization, space = fmt.fmt.pix.colorspace;
    if (enc == V4L2_YCBCR_ENC_DEFAULT) enc = V4L2_MAP_YCBCR_ENC_DEFAULT(space);
    if (quant == V4L2_QUANTIZATION_DEFAULT) quant = V4L2_MAP_QUANTIZATION_DEFAULT(0, space, enc);
    c.bt709 = enc == V4L2_YCBCR_ENC_709;
    c.full_range = quant == V4L2_QUANTIZATION_FULL_RANGE;

    struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    parm.parm.capture.timeperframe = interval;
    xioctl(c.fd, VIDIOC_S_PARM, &parm);   // best effort: some drivers have a single fixed rate
    double fps = parm.parm.capture.timeperframe.numerator
               ? (double)parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator : 0;
    SDL_snprintf(c.mode, sizeof c.mode, "%dx%d %c%c%c%c %.0f fps", c.w, c.h, (char)(c.pixfmt & 0xFF),
                 (char)((c.pixfmt >> 8) & 0xFF), (char)((c.pixfmt >> 16) & 0xFF), (char)((c.pixfmt >> 24) & 0xFF), fps);

    struct v4l2_requestbuffers req = { .count = MAX_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(c.fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) return fail("requesting buffers");
    c.buffer_count = (int)SDL_min(req.count, (uint32_t)MAX_BUFFERS);
    for (int i = 0; i < c.buffer_count; i++) {
        struct v4l2_buffer buf = { .index = (uint32_t)i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(c.fd, VIDIOC_QUERYBUF, &buf) < 0) return fail("querying a buffer");
        void *map = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, c.fd, buf.m.offset);
        if (map == MAP_FAILED) return fail("mapping a buffer");
        c.buffers[i].start = map;
        c.buffers[i].length = buf.length;
        if (xioctl(c.fd, VIDIOC_QBUF, &buf) < 0) return fail("queueing a buffer");
    }

#ifdef HAVE_TURBOJPEG
    if (is_jpeg(c.pixfmt) && !(c.jpeg = tj3Init(TJINIT_DECOMPRESS))) { errno = ENOMEM; return fail("starting the JPEG decoder"); }
#endif
    if (!c.lock) c.lock = SDL_CreateMutex();
    if (!c.lock || pipe2(c.wake, O_CLOEXEC) < 0) return fail("starting capture");

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c.fd, VIDIOC_STREAMON, &type) < 0) return fail("starting the stream");
    c.thread = SDL_CreateThread(capture_thread, "picashot-camera", NULL);
    if (!c.thread) { errno = EAGAIN; return fail("starting the capture thread"); }
    return true;
}

static bool v4l2_acquire(CamFrame *out) {
    if (c.fd < 0) return false;
    SDL_LockMutex(c.lock);
    bool have = c.fresh && c.ready >= 0;
    if (have) {
        c.reading = c.ready;   // the thread will not write into this store until it is released
        c.ready = -1;
        c.fresh = false;
    }
    SDL_UnlockMutex(c.lock);
    if (!have) return false;
    const Store *s = &c.stores[c.reading];
    *out = (CamFrame){ .fmt = s->fmt, .plane = { s->plane[0], s->plane[1], s->plane[2] },
                       .pitch = { s->pitch[0], s->pitch[1], s->pitch[2] }, .stamp_ns = s->stamp_ns,
                       .arrived_ns = s->arrived_ns };
    return true;
}

static void v4l2_release(void) {
    if (!c.lock) return;
    SDL_LockMutex(c.lock);
    c.reading = -1;
    SDL_UnlockMutex(c.lock);
}

static const char *v4l2_mode(void) { return c.mode; }

const CameraBackend camera_backend_v4l2 = { v4l2_count, v4l2_name, v4l2_open, v4l2_close, v4l2_acquire, v4l2_release, v4l2_mode };
