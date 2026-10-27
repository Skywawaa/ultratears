// High frame rates for TOTK 1.4.3: the switches of the lifter patches (tools/lifter/patches.py) and the real-time frame
// step they read. Ported from MacNativeGames games/totk/runtime/game_patches_v143.c (MIT).
//  - TOTK_FPS60 (default 1): the present-interval controller may pick interval 1, so the game is no longer held to 30.
//  - TOTK_REALTIME (default 1): the game steps its time by the measured length of the last frames instead of by the
//    interval it chose, so game speed (and physics per second) stays the same at 30, 60, 144 or 360 fps. The game's own
//    variable-frame-rate path does the stepping (the one it uses when it drops to 20 fps); only the step length changes.
//  - TOTK_CLOTH_REALTIME (default 1): cloth and hair use the measured step too (otherwise 4x fast at 120 fps).
//  - TOTK_HIGHFPS_FIX (default 1): trampolines and bouncy contacts compare the time scale, not the frame rate.
// The step is published at every present (hle_nvn.c nvnQueuePresentTexture -> runtime_frame_step_update).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// {30 Hz ratio, seconds, 60 Hz ratio}: read by the game's code (guest addresses are host addresses)
float totk_frame_step[4] = {0.5f, 1.0f / 60.0f, 1.0f};

static int flag(const char *name, int def, int *cache) {
    if (*cache < 0) {
        const char *e = getenv(name);
        *cache = e && *e ? atoi(e) != 0 : def;
        fprintf(stderr, "[totk] %s=%d%s\n", name, *cache, e && *e ? "" : " (default)");
    }
    return *cache;
}
int totk_fps60(void) { static int v = -1; return flag("TOTK_FPS60", 1, &v); }
int totk_realtime(void) { static int v = -1; return flag("TOTK_REALTIME", 1, &v); }
int totk_cloth_realtime(void) { static int v = -1; return totk_realtime() && flag("TOTK_CLOTH_REALTIME", 1, &v); }
int totk_highfps_fix(void) { static int v = -1; return flag("TOTK_HIGHFPS_FIX", 1, &v); }

// TOTK_FPS_LIMIT: the frame rate ceiling. "display" (default): the display's refresh rate (60, 144, 360 Hz...); a number
// (30..1000); 0 or "unlimited": 1000. The step is measured, so any rate keeps game speed; above the display's rate the
// newest frame wins at each refresh (lower latency, more GPU work).
int totk_fps_limit(int display_hz) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_FPS_LIMIT");
        if (!e || !*e || !strcmp(e, "display")) v = 0;
        else if (!strcmp(e, "unlimited") || !strcmp(e, "0")) v = 1000;
        else { v = atoi(e); v = v < 30 ? 30 : v > 1000 ? 1000 : v; }
        fprintf(stderr, "[totk] frame rate limit: %s\n", e && *e ? e : "display (TOTK_FPS_LIMIT)");
    }
    return v ? v : display_hz > 0 ? display_hz : 60;
}

static void store(float seconds) {
    float v[3] = {seconds * 30.0f, seconds, seconds * 60.0f};
    for (int i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], 4);
        __atomic_store_n((uint32_t *)&totk_frame_step[i], bits, __ATOMIC_RELEASE);
    }
}

// The mean of the last five present intervals. No lower bound (a floor such as 1/120 s made the game run fast above
// it: 1.2x at 144 fps, 3x at 360); a hitch counts at most 1/15 s, so a long stall slows the game instead of making one
// step large enough to break physics. Intervals below 1 ms or above 0.25 s (loading, pause) restart the average.
void runtime_frame_step_update(uint64_t now_ns) {
    static uint64_t last;
    static double samples[5];
    static unsigned count, next;
    if (!totk_realtime() || !now_ns) return;
    if (!last) { last = now_ns; return; }
    double dt = (double)(now_ns - last) * 1e-9;
    last = now_ns;
    if (dt > 0.25 || dt < 0.001) { count = next = 0; return; }
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    samples[next++ % 5] = dt;
    if (count < 5) count++;
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += samples[i];
    store((float)(sum / count));
}
