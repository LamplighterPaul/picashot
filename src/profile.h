// Built-in profiler. Run with PICASHOT_PROFILE=1 and every two seconds it prints, for that window:
// camera frame rate, how long each stage takes on the CPU and on the GPU (median, 99th percentile,
// worst), capture-to-screen latency, CPU per thread, the ffmpeg child's CPU while recording, and memory.
// Each report also ends with one "PROF key=value ..." line that tools/bench reads.
#ifndef PICASHOT_PROFILE_H
#define PICASHOT_PROFILE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROF_CAMERA,        // CPU: taking a camera frame and copying it to the GPU staging memory
    PROF_DRAW,          // CPU: recording and submitting a frame that is only shown
    PROF_DRAW_CAPTURE,  // CPU: the same, plus waiting for the picture to come back for a photo or video
    PROF_VIDEO_COPY,    // CPU: handing a frame to the video thread
    PROF_LATENCY,       // frame reached us from the driver -> frame submitted to the GPU
    PROF_LATENCY_SENSOR,// camera started the frame -> frame submitted to the GPU (includes the USB transfer)
    PROF_GPU_UPLOAD,    // GPU: camera frame into its image (and colour conversion, if any)
    PROF_GPU_FILTER,    // GPU: the filter pass
    PROF_GPU_READBACK,  // GPU: copying the picture out for a photo or video
    PROF_GPU_PRESENT,   // GPU: the window pass
    PROF_COUNT
} ProfSeries;

extern bool profile_on;
void     profile_init(void);                          // call first thing in main()
uint64_t profile_now(void);                           // a timestamp to pass to profile_since
void     profile_since(ProfSeries s, uint64_t since); // records the time since `since`
void     profile_ms(ProfSeries s, double ms);         // records a duration measured elsewhere
void     profile_camera_frame(void);                  // counts one camera frame
void     profile_first_picture(void);                 // call when the first camera frame has been drawn
void     profile_recording(bool on, int ffmpeg_pid, int dropped_frames);
void     profile_gpu_memory(uint64_t bytes);          // total device memory the renderer holds
void     profile_report(void);                        // prints when two seconds have passed

#endif
