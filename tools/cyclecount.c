// Counts the CPU work a running process does: user-space instructions and cycles per second, summed
// over all its threads. Unlike "percent of a core" these do not change when the CPU changes speed to
// save power, so two runs can be compared fairly.   Usage: cyclecount PID SECONDS
// Threads that start after counting begins are not included. Kernel time is not included.
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// On CPUs with two kinds of core (Intel's performance and efficiency cores) each kind has its own
// counters, and a thread has to be counted on both. Returns the kernel's id for one kind, or -1.
static int pmu_type(const char *name) {
    char path[128];
    int type = -1;
    snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/type", name);
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%d", &type) != 1) type = -1; fclose(f); }
    return type;
}

static int counter(pid_t tid, uint64_t config, int pmu) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = pmu >= 0 ? config | ((uint64_t)pmu << 32) : config;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: cyclecount PID SECONDS\n"); return 2; }
    int pid = atoi(argv[1]);
    double seconds = atof(argv[2]);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *dir = opendir(path);
    if (!dir) { perror(path); return 1; }
    int pmus[2] = { pmu_type("cpu_core"), pmu_type("cpu_atom") }, pmu_count = pmus[0] >= 0 && pmus[1] >= 0 ? 2 : 1;
    if (pmu_count == 1) pmus[0] = -1;
    static int fds[2048][2], tids[2048];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n + pmu_count <= 2048) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        for (int k = 0; k < pmu_count; k++) {
            fds[n][0] = counter(tid, PERF_COUNT_HW_INSTRUCTIONS, pmus[k]);
            fds[n][1] = counter(tid, PERF_COUNT_HW_CPU_CYCLES, pmus[k]);
            tids[n] = tid;
            if (fds[n][0] >= 0 && fds[n][1] >= 0) n++;
        }
    }
    closedir(dir);
    if (n == 0) { perror("perf_event_open"); return 1; }
    usleep((useconds_t)(seconds * 1e6));
    uint64_t instructions = 0, cycles = 0, v;
    for (int i = 0; i < n; i++) {
        uint64_t ti = 0, tc = 0;
        if (read(fds[i][0], &v, sizeof v) == sizeof v) ti = v;
        if (read(fds[i][1], &v, sizeof v) == sizeof v) tc = v;
        instructions += ti;
        cycles += tc;
        // With CYCLECOUNT_THREADS set, also list each thread that did real work, on stderr.
        if (getenv("CYCLECOUNT_THREADS") && ti / seconds > 1e6) {
            char name[64] = "?";
            snprintf(path, sizeof path, "/proc/%d/task/%d/comm", pid, tids[i]);
            FILE *f = fopen(path, "r");
            if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
            fprintf(stderr, "  %-18s %7.0f M instr/s %7.0f M cycles/s\n", name, (double)ti / seconds / 1e6, (double)tc / seconds / 1e6);
        }
    }
    printf("%.0f %.0f\n", (double)instructions / seconds / 1e6, (double)cycles / seconds / 1e6);
    return 0;
}
