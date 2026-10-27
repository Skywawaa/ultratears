// The benchmark (TOTK_BENCH=<seconds>, bench.bat): the same scene, the same camera, the same length every run, so that
// changes to the port can be compared by numbers (as nx-optimizer / UltraCam's benchmark). It
//  1. loads the newest save of the profile (TOTK_PROFILE_DIR; bench.bat uses bench\userdata, a copy made once with
//     bench-setup.bat, so the scene stays the one saved there): once the game has read the slots' captions, A is
//     pressed every 1.5 s on the title until the game opens a slot's progress.sav (MacNativeGames runtime/autoload.c);
//  2. waits until frames flow (120 frames under 100 ms in a row), then TOTK_BENCH_WARMUP seconds (default 10);
//  3. turns the camera once around the player at a steady rate for the measured seconds (tools/lifter/patches.py
//     bench_cam: the game's own eye is rotated about the vertical axis through the target, nothing else changes);
//  4. writes bench\bench-<date>-<time>.txt (frame rate, 1% / 0.1% lows, percentiles, hitches, the settings) and a line
//     of bench\results.csv, then quits.
// The keyboard and controller still work; pressing anything during the run makes the result meaningless, not wrong.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <direct.h>

#include "host.h"
#include "../runtime/cpu.h"

void runtime_input_key(unsigned code, int down);
const char *vkb_gpu_name(void);

enum { B_OFF, B_WAIT_SAVE, B_PRESS, B_LOADING, B_WARMUP, B_RUN, B_DONE };
static volatile int g_state = B_OFF;
static double g_seconds, g_warmup = 10;
static uint64_t g_press_at, g_phase_t0, g_last_ns;
static unsigned g_steady;
static float *g_ft;   // frame times (ms) of the measured run
static size_t g_nft, g_capft;
static volatile float g_angle;   // the camera's turn (radians), read by bench_cam

__attribute__((constructor)) static void bench_configure(void) {
    const char *e = getenv("TOTK_BENCH");
    if (!e || !*e || !strcmp(e, "0")) return;
    g_seconds = atof(e);
    if (g_seconds < 5 || g_seconds > 3600) g_seconds = 60;
    const char *w = getenv("TOTK_BENCH_WARMUP");
    if (w && *w) g_warmup = atof(w);
    g_state = B_WAIT_SAVE;
    fprintf(stderr, "[bench] on: %.0f s measured after %.0f s of warm-up; the newest save of the profile is loaded\n", g_seconds, g_warmup);
}

// fs.c, at every file the game opens: the title has read the slots (press A), a slot's game state is being loaded
void runtime_bench_file(const char *path) {
    if (g_state == B_OFF || !path) return;
    if (g_state == B_WAIT_SAVE && strstr(path, "caption.sav")) { g_state = B_PRESS; g_press_at = 0; fprintf(stderr, "[bench] save found: pressing A on the title\n"); }
    else if (g_state == B_PRESS && strstr(path, "progress.sav")) {
        g_state = B_LOADING; g_steady = 0;
        runtime_input_key(38, 0);
        fprintf(stderr, "[bench] loading the save\n");
    }
}

static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void report(void) {
    _mkdir("bench");
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    char name[96], stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    strftime(name, sizeof name, "bench/bench-%Y%m%d-%H%M%S.txt", &tm);
    size_t n = g_nft;
    float *s = malloc(n * sizeof *s);
    if (!s || !n) return;
    memcpy(s, g_ft, n * sizeof *s);
    qsort(s, n, sizeof *s, fcmp);
    double sum = 0, worst1 = 0, worst01 = 0;
    for (size_t i = 0; i < n; i++) sum += s[i];
    size_t k1 = n / 100 ? n / 100 : 1, k01 = n / 1000 ? n / 1000 : 1;
    for (size_t i = 0; i < k1; i++) worst1 += s[n - 1 - i];
    for (size_t i = 0; i < k01; i++) worst01 += s[n - 1 - i];
    unsigned h50 = 0, h100 = 0;
    for (size_t i = 0; i < n; i++) { h50 += s[i] > 50; h100 += s[i] > 100; }
    double avg = 1000.0 * n / sum, low1 = 1000.0 * k1 / worst1, low01 = 1000.0 * k01 / worst01;
#define P(q) s[(size_t)((n - 1) * (q))]
    static const char *vars[] = {"TOTK_FPS_LIMIT", "TOTK_PRESENT_MODE", "TOTK_REFLEX", "TOTK_RENDER_RES", "TOTK_OUTPUT_RES",
                                 "TOTK_FPS60", "TOTK_ONE_PER_CORE", "TOTK_CORES", "TOTK_CORE_PREEMPT", "TOTK_SPIN_HANDOFF_US",
                                 "TOTK_TEX_THREADS", "TOTK_KLOCK_SPIN"};
    char settings[1024] = "";
    for (unsigned i = 0; i < sizeof vars / sizeof *vars; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) { strncat(settings, vars[i], sizeof settings - strlen(settings) - 1); strncat(settings, "=", sizeof settings - strlen(settings) - 1);
                       strncat(settings, v, sizeof settings - strlen(settings) - 1); strncat(settings, " ", sizeof settings - strlen(settings) - 1); }
    }
    char text[2048];
    snprintf(text, sizeof text,
             "UltraTears benchmark %s\nGPU: %s\nsettings: %s\nmeasured: %.0f s, %zu frames\n\n"
             "average      %7.1f fps\n1%% low       %7.1f fps\n0.1%% low     %7.1f fps\n"
             "frame time   p50 %.2f ms, p95 %.2f ms, p99 %.2f ms, max %.1f ms\nhitches      %u over 50 ms, %u over 100 ms\n",
             stamp, vkb_gpu_name(), *settings ? settings : "(defaults)", g_seconds, n, avg, low1, low01, P(0.5), P(0.95), P(0.99), s[n - 1], h50, h100);
    FILE *f = fopen(name, "w");
    if (f) { fputs(text, f); fclose(f); }
    int fresh = 0;
    FILE *c = fopen("bench/results.csv", "r");
    if (c) fclose(c); else fresh = 1;
    if ((c = fopen("bench/results.csv", "a"))) {
        if (fresh) fprintf(c, "date,avg_fps,low1_fps,low01_fps,p50_ms,p95_ms,p99_ms,max_ms,hitches50,hitches100,frames,seconds,settings\n");
        fprintf(c, "%s,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.1f,%u,%u,%zu,%.0f,%s\n", stamp, avg, low1, low01, P(0.5), P(0.95), P(0.99), s[n - 1], h50, h100, n, g_seconds, settings);
        fclose(c);
    }
#undef P
    fprintf(stderr, "[bench] result (%s):\n%s", name, text);
    free(s);
}

// hle_nvn.c, at every present
void runtime_bench_frame(uint64_t now) {
    int st = g_state;
    if (st == B_OFF || st == B_DONE || st == B_WAIT_SAVE) return;
    double ms = g_last_ns ? (now - g_last_ns) / 1e6 : 0;
    g_last_ns = now;
    if (st == B_PRESS) {   // A on the title, held 150 ms every 1.5 s
        if (!g_press_at || now - g_press_at > 1500000000ull) { g_press_at = now; runtime_input_key(38, 1); }
        else if (now - g_press_at > 150000000ull) runtime_input_key(38, 0);
        return;
    }
    if (st == B_LOADING) {
        g_steady = ms > 0 && ms < 100 ? g_steady + 1 : 0;
        if (g_steady >= 120) { g_state = B_WARMUP; g_phase_t0 = now; fprintf(stderr, "[bench] in the world: %.0f s of warm-up\n", g_warmup); }
        return;
    }
    if (st == B_WARMUP) {
        if ((now - g_phase_t0) / 1e9 >= g_warmup) { g_state = B_RUN; g_phase_t0 = now; g_nft = 0; fprintf(stderr, "[bench] measuring %.0f s\n", g_seconds); }
        return;
    }
    double t = (now - g_phase_t0) / 1e9;
    {   // TOTK_BENCH_SHOTS=1: the picture at 0, 1/4, 1/2 and 3/4 of the run (bench\shot-<n>.bmp), to see the camera turn
        static int shots = -1, taken;
        if (shots < 0) { const char *e = getenv("TOTK_BENCH_SHOTS"); shots = e && *e == '1'; }
        if (shots && taken < 4 && t >= g_seconds * taken / 4) {
            void vkb_request_shot(const char *path);
            char p[64];
            snprintf(p, sizeof p, "bench/shot-%d.bmp", taken++);
            vkb_request_shot(p);
        }
    }
    if (g_nft == g_capft) { g_capft = g_capft ? g_capft * 2 : 65536; g_ft = realloc(g_ft, g_capft * sizeof *g_ft); if (!g_ft) { g_state = B_DONE; return; } }
    if (ms > 0) g_ft[g_nft++] = (float)ms;
    g_angle = (float)(2 * M_PI * t / g_seconds);
    if (t >= g_seconds) {
        g_state = B_DONE;
        report();
        fflush(stderr);
        _Exit(0);
    }
}

/* tools/lifter/patches.py bench_cam, at main+0x628828: the camera update has stored this frame's eye (doubles d1, d2, d0;
   the camera's +0x60.. and floats +0x38..) and target (d3, d4, d5) and builds the view from those registers next. While
   the benchmark measures, the eye turns about the vertical axis through the target by g_angle: the same distance and
   height, the view all around the player. x20 is the camera. */
static double getd(const Cpu *c, int n) { uint64_t b = (uint64_t)c->v[n]; double d; memcpy(&d, &b, 8); return d; }
static void setd(Cpu *c, int n, double d) { uint64_t b; memcpy(&b, &d, 8); c->v[n] = b; }
void totk_bench_cam(Cpu *c) {
    if (g_state != B_RUN) return;
    double ex = getd(c, 1), ey = getd(c, 2), ez = getd(c, 0), tx = getd(c, 3), tz = getd(c, 5);
    double a = g_angle, dx = ex - tx, dz = ez - tz;
    double nx = tx + dx * cos(a) - dz * sin(a), nz = tz + dx * sin(a) + dz * cos(a);
    setd(c, 1, nx);
    setd(c, 0, nz);
    uint64_t cam = c->x[20];
    double d[3] = {nx, ey, nz};
    float f[3] = {(float)nx, (float)ey, (float)nz};
    memcpy((void *)(uintptr_t)(cam + 0x60), d, sizeof d);
    memcpy((void *)(uintptr_t)(cam + 0x38), f, sizeof f);
}
