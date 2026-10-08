// Saving: photos as PNG, videos through an ffmpeg child process. Both work in the background so the
// preview never stalls.
#ifndef PICASHOT_CAPTURE_H
#define PICASHOT_CAPTURE_H
#include <stdbool.h>
#include <stdint.h>

// Copies the picture (w*h*4 RGBA bytes) and saves it to Pictures/Picashot in the background.
void photo_save(const uint8_t *rgba, int w, int h);

// Finds the best working video encoder (a GPU one where there is one) on a background thread, so the
// first recording starts at once. `forced` names one to use without testing ("x264", "vaapi", ...),
// or is NULL. Safe to call more than once.
void video_prepare(const char *forced);
const char *video_encoder_name(void);    // "h264_vaapi", "libx264", ...; "" until known

// Starts ffmpeg writing to Videos/Picashot. Frames are w*h*3/2 bytes of NV12 (BT.709, limited range).
// Returns false if ffmpeg could not be started.
bool video_start(int w, int h, bool with_audio);
// Hands over one frame. Frames are dropped, not queued forever, if the encoder falls behind.
void video_frame(const uint8_t *nv12);
// True when the queue has room. Checking first saves the GPU work for a frame that would be dropped.
bool video_wants_frame(void);
void video_stop(void);
bool video_recording(void);
// For the profiler: the ffmpeg process id (0 when not recording) and frames dropped in this recording.
int  video_encoder_pid(void);
int  video_dropped_frames(void);

// Blocks until every photo and video has finished writing. Call before exit.
void capture_finish(void);
// Set to hide desktop notifications (used by the scripted --snap and --record modes).
extern bool capture_quiet;

#endif
