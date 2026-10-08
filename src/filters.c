// SPIR-V for every shader, compiled by the Makefile into build/spv/*.inc and built into the binary,
// so the installed program is a single file.
#include "filters.h"

const uint32_t g_spv_fullscreen_vert[] =
#include "fullscreen.vert.inc"
;
const size_t g_spv_fullscreen_vert_bytes = sizeof g_spv_fullscreen_vert;

const uint32_t g_spv_present_frag[] =
#include "present.frag.inc"
;
const size_t g_spv_present_frag_bytes = sizeof g_spv_present_frag;

const uint32_t g_spv_yuv2rgb_frag[] =
#include "yuv2rgb.frag.inc"
;
const size_t g_spv_yuv2rgb_frag_bytes = sizeof g_spv_yuv2rgb_frag;

const uint32_t g_spv_video_y_frag[] =
#include "video_y.frag.inc"
;
const size_t g_spv_video_y_frag_bytes = sizeof g_spv_video_y_frag;

const uint32_t g_spv_video_uv_frag[] =
#include "video_uv.frag.inc"
;
const size_t g_spv_video_uv_frag_bytes = sizeof g_spv_video_uv_frag;

// build/spv/filters_spv.inc holds one `static const uint32_t spv_<name>[] = {...};` per filter.
#include "filters_spv.inc"

#define X(name, title) { #name, title, spv_##name, sizeof spv_##name },
const Filter g_filters[] = { FILTER_LIST(X) };
#undef X
const int g_filter_count = (int)(sizeof g_filters / sizeof g_filters[0]);
