// Camera capture. Two backends behind one interface:
//   v4l2: our own. Reads the device directly, and decodes MJPEG with libjpeg-turbo straight to YUV
//         planes on a capture thread. Colour conversion happens on the GPU. This is the fast path.
//   sdl:  SDL3's camera API, which also reaches cameras that only exist through PipeWire. It hands
//         over RGBA and costs more CPU. Used when the v4l2 backend cannot open the camera.
#ifndef PICASHOT_CAMERA_H
#define PICASHOT_CAMERA_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAM_RGBA,    // plane 0: R,G,B,A bytes
    CAM_NV12,    // plane 0: Y; plane 1: U,V interleaved at half width and half height
    CAM_YUYV,    // plane 0: Y,U,Y,V bytes (two bytes per pixel)
    CAM_PLANAR,  // plane 0: Y; plane 1: U; plane 2: V, the chroma planes at chroma_w x chroma_h
} CamLayout;

typedef struct {
    int       w, h;
    CamLayout layout;
    int       chroma_w, chroma_h;   // size of the chroma plane(s) for CAM_NV12 and CAM_PLANAR
    bool      full_range;           // YUV uses 0..255 (JPEG) and not 16..235
    bool      bt709;                // BT.709 matrix and not BT.601
} CamFormat;

typedef struct {
    CamFormat      fmt;
    const uint8_t *plane[3];
    int            pitch[3];        // bytes from one row to the next
    uint64_t       stamp_ns;        // when the camera started the frame, on CLOCK_MONOTONIC (driver's word)
    uint64_t       arrived_ns;      // when the frame reached this process, before any decoding
} CamFrame;

// Lists cameras. Call once at start.
int         camera_count(void);
const char *camera_name(int index);

// Opens a camera, preferring a mode close to want_w x want_h at 30 fps. `force_sdl` skips the v4l2 backend.
bool        camera_open(int index, int want_w, int want_h, bool force_sdl);
void        camera_close(void);
bool        camera_is_open(void);
const char *camera_backend(void);       // "v4l2" or "sdl"
const char *camera_mode(void);          // e.g. "1920x1080 MJPG 30 fps", for logs

// Gives the newest frame if one arrived since the last call. The planes stay valid until camera_release().
bool        camera_acquire(CamFrame *out);
void        camera_release(void);

// The v4l2 backend pushes an SDL event of this type whenever a frame is ready, so the main loop can
// sleep until then. 0 for the sdl backend, which has to be polled.
uint32_t    camera_wake_event(void);

// ---- backend entry points (camera_v4l2.c, camera_sdl.c) ----
typedef struct {
    int         (*count)(void);
    const char *(*name)(int index);
    bool        (*open)(int index, int want_w, int want_h);
    void        (*close)(void);
    bool        (*acquire)(CamFrame *out);
    void        (*release)(void);
    const char *(*mode)(void);
} CameraBackend;
extern const CameraBackend camera_backend_v4l2, camera_backend_sdl;
extern uint32_t camera_event_type;      // registered by camera.c

uint64_t camera_monotonic_ns(void);
// Shared mode scoring: larger is better. Prefers sizes up to the wanted one and rates near 30 fps.
long camera_score_mode(int w, int h, double fps, int want_w, int want_h);

#endif
