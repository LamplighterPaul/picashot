// The list of filters. To add one: write shaders/<name>.frag, then add a line to FILTER_LIST.
#ifndef PICASHOT_FILTERS_H
#define PICASHOT_FILTERS_H
#include <stddef.h>
#include <stdint.h>

// X(file name without extension, title shown to the user). Order here is the order on screen.
#define FILTER_LIST(X) \
    X(normal,   "Normal")   \
    X(noir,     "Noir")     \
    X(sepia,    "Sepia")    \
    X(dream,    "Dream")    \
    X(comic,    "Comic")    \
    X(pop,      "Pop")      \
    X(neon,     "Neon")     \
    X(halftone, "Halftone") \
    X(pixel,    "Pixel")    \
    X(thermal,  "Thermal")  \
    X(xray,     "X-Ray")    \
    X(glitch,   "Glitch")   \
    X(bulge,    "Bulge")    \
    X(pinch,    "Pinch")    \
    X(twirl,    "Twirl")    \
    X(kaleido,  "Mirror")

typedef struct {
    const char     *name;    // shaders/<name>.frag
    const char     *title;
    const uint32_t *spv;     // SPIR-V built into the binary
    size_t          spv_bytes;
} Filter;

extern const Filter g_filters[];
extern const int    g_filter_count;

extern const uint32_t g_spv_fullscreen_vert[];
extern const size_t   g_spv_fullscreen_vert_bytes;
extern const uint32_t g_spv_present_frag[];
extern const size_t   g_spv_present_frag_bytes;
extern const uint32_t g_spv_yuv2rgb_frag[];
extern const size_t   g_spv_yuv2rgb_frag_bytes;
extern const uint32_t g_spv_video_y_frag[];
extern const size_t   g_spv_video_y_frag_bytes;
extern const uint32_t g_spv_video_uv_frag[];
extern const size_t   g_spv_video_uv_frag_bytes;

#endif
