#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "profile.h"

#define REPORT_MS   2000
#define MAX_THREADS 64
#define MAX_SAMPLES 512

bool profile_on;

static const char *series_names[PROF_COUNT] = {
    "cpu camera copy", "cpu draw", "cpu draw+capture", "cpu video copy", "latency arrive->gpu", "latency sensor->gpu",
    "gpu upload", "gpu filter", "gpu readback", "gpu present",
};
static const char *series_keys[PROF_COUNT] = {
    "cam_copy", "draw", "draw_capture", "video_copy", "latency", "latency_sensor", "gpu_upload", "gpu_filter", "gpu_readback", "gpu_present",
};

static struct {
    float    samples[PROF_COUNT][MAX_SAMPLES];
    int      counts[PROF_COUNT];
    int      camera_frames;
    Uint64   last_report, started;
    struct { int tid; unsigned long cpu; } threads[MAX_THREADS];   // CPU ticks at the last report
    int      thread_count;
    bool     recording;
    int      ffmpeg_pid, dropped;
    unsigned long ffmpeg_cpu;
    uint64_t gpu_bytes;
    double   launch_ms;        // exec -> first picture; 0 until known
} p;

// Milliseconds since boot, on the clock /proc uses for process start times (it counts suspended time).
static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

void profile_init(void) {
    profile_on = SDL_getenv("PICASHOT_PROFILE") != NULL;
    p.last_report = p.started = SDL_GetTicks();
}

uint64_t profile_now(void) { return profile_on ? SDL_GetPerformanceCounter() : 0; }

void profile_ms(ProfSeries s, double ms) {
    if (!profile_on || p.counts[s] >= MAX_SAMPLES) return;
    p.samples[s][p.counts[s]++] = (float)ms;
}

void profile_since(ProfSeries s, uint64_t since) {
    if (!profile_on) return;
    profile_ms(s, (double)(SDL_GetPerformanceCounter() - since) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

void profile_camera_frame(void) { p.camera_frames++; }
void profile_gpu_memory(uint64_t bytes) { p.gpu_bytes = bytes; }

void profile_recording(bool on, int ffmpeg_pid, int dropped_frames) {
    if (on && !p.recording) p.ffmpeg_cpu = 0;
    p.recording = on;
    p.ffmpeg_pid = ffmpeg_pid;
    p.dropped = dropped_frames;
}

// Reads the name and the CPU ticks used so far from a /proc/.../stat file. `start` gets the start time
// in clock ticks since boot when it is not NULL.
static bool read_stat(const char *path, unsigned long *cpu, char *name, size_t name_size, unsigned long long *start) {
    char line[1024];
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    char *open = ok ? strchr(line, '(') : NULL, *close = ok ? strrchr(line, ')') : NULL;
    if (!open || !close) return false;
    *close = 0;
    if (name) SDL_strlcpy(name, open + 1, name_size);
    unsigned long utime = 0, stime = 0;
    unsigned long long st = 0;
    // After the name: state, 10 fields we skip, utime, stime, 6 more we skip, then starttime.
    if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %llu",
               &utime, &stime, &st) != 3) return false;
    *cpu = utime + stime;
    if (start) *start = st;
    return true;
}

void profile_first_picture(void) {
    if (!profile_on || p.launch_ms > 0) return;
    unsigned long cpu;
    unsigned long long start_ticks;
    if (!read_stat("/proc/self/stat", &cpu, NULL, 0, &start_ticks)) return;
    p.launch_ms = monotonic_ms() - (double)start_ticks * 1000.0 / (double)sysconf(_SC_CLK_TCK);
    printf("-- profile: launch to first picture %.0f ms\nPROF launch_ms=%.0f\n", p.launch_ms, p.launch_ms);
    fflush(stdout);
}

static long status_kb(const char *key) {
    char line[256];
    long kb = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) { kb = atol(line + n + 1); break; }
    fclose(f);
    return kb;
}

static int compare_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void profile_report(void) {
    if (!profile_on) return;
    Uint64 now = SDL_GetTicks();
    if (now - p.last_report < REPORT_MS) return;
    double secs = (double)(now - p.last_report) / 1000.0;
    p.last_report = now;

    char line[1024];
    int at = SDL_snprintf(line, sizeof line, "PROF t=%.0f rec=%d fps=%.1f", (double)(now - p.started) / 1000.0,
                          p.recording, p.camera_frames / secs);
    printf("-- profile: %.1f camera fps%s\n", p.camera_frames / secs, p.recording ? ", recording" : "");
    p.camera_frames = 0;

    for (int s = 0; s < PROF_COUNT; s++) {
        int n = p.counts[s];
        p.counts[s] = 0;
        if (!n) continue;
        qsort(p.samples[s], (size_t)n, sizeof(float), compare_float);
        float p50 = p.samples[s][n / 2], p99 = p.samples[s][(n * 99) / 100], worst = p.samples[s][n - 1];
        printf("   %-20s p50 %6.2f  p99 %6.2f  max %6.2f ms  x%d\n", series_names[s], p50, p99, worst, n);
        at += SDL_snprintf(line + at, sizeof line - (size_t)at, " %s_p50=%.2f %s_p99=%.2f", series_keys[s], p50, series_keys[s], p99);
    }

    long hz = sysconf(_SC_CLK_TCK);
    double total = 0, biggest = 0, main_pct = 0;
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    int self = (int)getpid();
    while (dir && (e = readdir(dir))) {
        int tid = atoi(e->d_name);
        unsigned long cpu;
        char name[32], path[64];
        SDL_snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        if (tid <= 0 || !read_stat(path, &cpu, name, sizeof name, NULL)) continue;
        int slot = -1;
        for (int i = 0; i < p.thread_count; i++) if (p.threads[i].tid == tid) slot = i;
        if (slot < 0 && p.thread_count < MAX_THREADS) { slot = p.thread_count++; p.threads[slot].tid = tid; p.threads[slot].cpu = cpu; continue; }
        if (slot < 0) continue;
        double pct = (double)(cpu - p.threads[slot].cpu) * 100.0 / ((double)hz * secs);
        p.threads[slot].cpu = cpu;
        total += pct;
        if (tid == self) main_pct = pct;
        else if (pct > biggest) biggest = pct;
        if (pct >= 0.5) printf("   thread %-16s %5.1f%% of one core\n", tid == self ? "main" : name, pct);
    }
    if (dir) closedir(dir);
    printf("   all threads          %5.1f%% of one core\n", total);
    at += SDL_snprintf(line + at, sizeof line - (size_t)at, " cpu=%.1f cpu_main=%.1f cpu_top_thread=%.1f", total, main_pct, biggest);

    if (p.recording && p.ffmpeg_pid > 0) {
        char path[64];
        unsigned long cpu;
        SDL_snprintf(path, sizeof path, "/proc/%d/stat", p.ffmpeg_pid);
        if (read_stat(path, &cpu, NULL, 0, NULL)) {
            double pct = p.ffmpeg_cpu ? (double)(cpu - p.ffmpeg_cpu) * 100.0 / ((double)hz * secs) : 0;
            p.ffmpeg_cpu = cpu;
            printf("   ffmpeg child         %5.1f%% of one core, %d frame(s) dropped so far\n", pct, p.dropped);
            at += SDL_snprintf(line + at, sizeof line - (size_t)at, " cpu_ffmpeg=%.1f dropped=%d", pct, p.dropped);
        }
    }

    long rss = status_kb("VmRSS:"), peak = status_kb("VmHWM:");
    printf("   memory               %ld MB now, %ld MB peak, %.0f MB on the GPU\n", rss / 1024, peak / 1024, (double)p.gpu_bytes / 1048576.0);
    SDL_snprintf(line + at, sizeof line - (size_t)at, " rss_mb=%ld peak_mb=%ld gpu_mb=%.0f", rss / 1024, peak / 1024, (double)p.gpu_bytes / 1048576.0);
    puts(line);
    fflush(stdout);
}
