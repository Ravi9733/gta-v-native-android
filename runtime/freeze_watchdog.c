/* Freeze watchdog (task 0k, after smo-recomp's nvn_vk_watchdog.c): when the game stops producing frames, write what is
   needed to see why, and nothing else. Off unless ODYSSEY_FREEZE_WATCHDOG is set: "1" = 5 s without a frame, "N" = N s.
   It never stops, resumes or changes the game; it only logs.

   Once the game has presented its first frame, a thread looks every 500 ms at the frames the game submitted
   (dm_nvn_frame_count, hle_nvn.c) and the frames actually presented (mtl_present_count, nvn_mtl.m). When neither has moved
   for the threshold it logs one "[freeze-watchdog]" line and dumps, once per freeze (at most 3 per run):
   - the guest threads, core owners and lock owners (runtime_dump_threads, threads.c), to stderr;
   - the GPU wait state (mtl_wait_state_dump, nvn_mtl.m), to stderr;
   - every host thread's stack: /usr/bin/sample for 3 s into <dir>/freeze-<pid>-<n>.txt, where <dir> is
     ODYSSEY_FREEZE_DIR, else freeze-dumps next to TOTK_PROFILE_DIR, else ./freeze-dumps.
   Background/resume edges reset the movement baseline and do not consume either budget. After three full dumps,
   each new foreground freeze still gets one safe thread-table/atomic GPU-wait snapshot (no host sampler or guest walk).
   When frames move again it logs how long the freeze lasted. The hooks are weak, so a target without them (the headless
   probe) still links; a missing hook is skipped. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <TargetConditionals.h>

extern char **environ;
__attribute__((weak)) uint64_t dm_nvn_frame_count(void);
__attribute__((weak)) uint64_t mtl_present_count(void);
__attribute__((weak)) void runtime_dump_threads(void);
__attribute__((weak)) void runtime_dump_threads_safe(void);
__attribute__((weak)) void mtl_wait_state_dump(void);
__attribute__((weak_import)) void mtl_wait_state_dump_safe(void);
/* Low bit: active; upper bits: activity-edge generation. This notices a full
   background/resume cycle even when iOS suspended the watchdog itself. */
__attribute__((weak_import)) uint64_t mtl_app_activity_token(void);

enum { FW_MAX_DUMPS = 3 };
static double g_threshold_s;
static double g_periodic_s;   /* TOTK_THREAD_DUMP=<s>: dump the threads every s seconds, no freeze needed */
static int g_periodic_left = 40;
static char g_dir[PATH_MAX];

static double fw_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
static uint64_t fw_frames(void) { return dm_nvn_frame_count ? dm_nvn_frame_count() : 0; }
static uint64_t fw_presents(void) { return mtl_present_count ? mtl_present_count() : 0; }
static uint64_t fw_activity(void) { return mtl_app_activity_token ? mtl_app_activity_token() : 1; }

static pid_t fw_sample(int n) {   /* every host thread's stack, by Apple's sampler, in its own process */
#if TARGET_OS_IPHONE
    (void)n; /* /usr/bin/sample cannot be spawned inside an iOS application. */
    return 0;
#else
    mkdir(g_dir, 0755);
    char path[PATH_MAX], pid[16];
    snprintf(path, sizeof path, "%s/freeze-%d-%d.txt", g_dir, (int)getpid(), n);
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *argv[] = {"/usr/bin/sample", pid, "3", "-file", path, "-mayDie", NULL};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK);
    sigset_t none; sigemptyset(&none); posix_spawnattr_setsigmask(&at, &none);
    pid_t child = 0;
    int err = posix_spawn(&child, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if (err) { fprintf(stderr, "[freeze-watchdog] could not start sample: %s\n", strerror(err)); return 0; }
    fprintf(stderr, "[freeze-watchdog] host thread stacks (3 s sample) -> %s\n", path);
    return child;
#endif
}

static void *fw_thread(void *unused) {
    (void)unused;
    pthread_setname_np("freeze-watchdog");
    uint64_t frames = fw_frames(), presents = fw_presents();
    uint64_t activity = fw_activity();
    double moved = fw_now(), frozen_at = 0;
    double periodic_last = 0;
    int dumps = 0; pid_t child = 0;
    for (;;) {
        usleep(500000);
        if (child && waitpid(child, NULL, WNOHANG) == child) child = 0;
        uint64_t f = fw_frames(), p = fw_presents();
        double now = fw_now();
        uint64_t a = fw_activity();
        if (!(a & 1) || a != activity) {
            activity = a; frames = f; presents = p;
            moved = now; frozen_at = 0; periodic_last = now;
            continue;
        }
        if (g_periodic_s > 0 && g_periodic_left > 0) {
            if (now - periodic_last >= g_periodic_s) {
                periodic_last = now; g_periodic_left--;
                fprintf(stderr, "[thread-dump] periodic %d (game frame %llu, present %llu)\n", 41 - g_periodic_left, (unsigned long long)f, (unsigned long long)p);
                if (runtime_dump_threads_safe) runtime_dump_threads_safe(); else if (runtime_dump_threads) runtime_dump_threads();
            }
        }
        if (!f && !p) { moved = now; continue; }   /* not armed before the first frame (startup, shader warm-up) */
        if (f != frames || p != presents) {
            if (frozen_at) fprintf(stderr, "[freeze-watchdog] frames moved again after %.1f s (game frame %llu, present %llu)\n",
                                   now - moved, (unsigned long long)f, (unsigned long long)p);
            frames = f; presents = p; moved = now; frozen_at = 0;
            continue;
        }
        if (g_threshold_s >= 1e9 || frozen_at || now - moved < g_threshold_s) continue;
        frozen_at = now;
        if (dumps >= FW_MAX_DUMPS) {
            fprintf(stderr, "[freeze-watchdog] no frame for %.1f s (game frame %llu, present %llu); full dump limit reached, safe snapshot\n",
                    now - moved, (unsigned long long)f, (unsigned long long)p);
            if (runtime_dump_threads_safe) runtime_dump_threads_safe();
            if (mtl_wait_state_dump_safe) mtl_wait_state_dump_safe();
            continue;
        }
        dumps++;
        fprintf(stderr, "[freeze-watchdog] no frame for %.1f s (game frame %llu, present %llu); dump %d of %d\n",
                now - moved, (unsigned long long)f, (unsigned long long)p, dumps, FW_MAX_DUMPS);
        if (!child) child = fw_sample(dumps);
        if (runtime_dump_threads) runtime_dump_threads();
        if (mtl_wait_state_dump) mtl_wait_state_dump();
    }
    return NULL;
}

__attribute__((constructor)) static void fw_start(void) {
    const char *e = getenv("ODYSSEY_FREEZE_WATCHDOG");
    const char *pd = getenv("TOTK_THREAD_DUMP");
    if (pd && *pd) { g_periodic_s = atof(pd); if (g_periodic_s < 1) g_periodic_s = 1; }
    if ((!e || !*e || !strcmp(e, "0")) && !(g_periodic_s > 0)) return;
    if (!e || !*e || !strcmp(e, "0")) g_threshold_s = 1e9;   /* periodic dump only: the freeze path (host sample, guest stack walk) stays off */
    else { double s = atof(e); g_threshold_s = s > 1 ? s : 5; }
    const char *dir = getenv("ODYSSEY_FREEZE_DIR"), *profile = getenv("TOTK_PROFILE_DIR");
    if (dir && *dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else if (profile && *profile) snprintf(g_dir, sizeof g_dir, "%s/../freeze-dumps", profile);
    else snprintf(g_dir, sizeof g_dir, "freeze-dumps");
    pthread_t t;
    if (pthread_create(&t, NULL, fw_thread, NULL)) { fprintf(stderr, "[freeze-watchdog] could not start: %s\n", strerror(errno)); return; }
    pthread_detach(t);
    fprintf(stderr, "[freeze-watchdog] on: logs and dumps after %.0f s without a frame (dumps in %s); never changes the game\n", g_threshold_s, g_dir);
}

/* TOTK_SHOT_AFTER=<s> TOTK_SHOT_PATH=<file>: after s seconds (e.g. once the world has loaded) write the
   shown window as a PNG — lets a headless/remote run (iOS over devicectl) capture what the screen shows. */
__attribute__((weak)) void dm_gfx_window_shot(const char *path);
static void *fw_shot_thread(void *p) {
    long s = (long)p;
    sleep((unsigned)s);
    const char *path = getenv("TOTK_SHOT_PATH");
    if (path && *path) {
        if (dm_gfx_window_shot) dm_gfx_window_shot(path);
        fprintf(stderr, "[shot-after] window screenshot after %ld s -> %s\n", s, path);
    }
    return NULL;
}
__attribute__((constructor)) static void fw_shot_start(void) {
    const char *e = getenv("TOTK_SHOT_AFTER");
    if (!e || !*e) return;
    pthread_t t;
    if (pthread_create(&t, NULL, fw_shot_thread, (void *)(long)atoi(e))) return;
    pthread_detach(t);
}
