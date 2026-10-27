// nn::vi (display, layer, native window, vsync event) for the recompiled game.
// Ported from MacNativeGames games/totk/runtime/frontend.m @b75be8c (MIT): the display and layer are opaque host
// identities; the native window is the host window object the game hands back to NVN (nvnWindowBuilderSetNativeWindow).
// The vsync event is signalled by a host thread at the display rate. Milestone 3.1: no window yet, a 60 Hz clock.
#include <stdatomic.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"

static int vi_ready;
static unsigned display_identity, layer_identity, window_identity;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vsync_events[16];
static unsigned vsync_count;

void *front_metal_layer(void) { return &window_identity; }   // milestone 3.1: the Vulkan surface comes in 3.2
void front_frame_presented(void) {}
uint64_t front_game_res(int docked) { (void)docked; return (720ULL << 32) | 1280; }

// [Windows port] at the display's refresh rate, or at the frame rate limit when that is higher (TOTK_FPS_LIMIT above the
// display: the game waits for these events, so they must come at least as often as frames are wanted).
extern int mtl_display_hz(void), totk_fps_limit(int display_hz);
static void *vsync_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-vsync");   // gfx_compat.h: the Darwin form
    int scr = mtl_display_hz(), hz = totk_fps_limit(scr) > scr ? totk_fps_limit(scr) : scr;
    const uint64_t period = 1000000000ull / (uint64_t)hz;
    fprintf(stderr, "[totk] vsync events at %d Hz (display %d Hz)\n", hz, scr);
    uint64_t next = runtime_now_ns() + period;
    for (;;) {
        plat_sleep_until_ns(next);
        next += period;
        pthread_mutex_lock(&vsync_lock);
        for (unsigned i = 0; i < vsync_count; i++) hle_signal_system_event(vsync_events[i]);
        pthread_mutex_unlock(&vsync_lock);
    }
    return NULL;
}

static void vi_initialize(void) {
    vi_ready = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, vsync_thread, NULL)) runtime_fail("vsync thread");
    fprintf(stderr, "[totk] VI display initialized\n");
}
static uint32_t vi_open(void **out) { if (!vi_ready || !out) return 0xf601; *out = &display_identity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &display_identity) return 0xf601; *out = &layer_identity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layer_identity) return 0xf601; *out = front_metal_layer(); return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &display_identity) return 0xf601;
    hle_bind_system_event(ev, 1);   // on the calling guest thread: creates the SDK's own system event
    pthread_mutex_lock(&vsync_lock);
    if (vsync_count >= 16) runtime_fail("VI display event limit");
    vsync_events[vsync_count++] = ev;
    pthread_mutex_unlock(&vsync_lock);
    return 0;
}

HLE_NATIVE("_ZN2nn2vi10InitializeEv", vi_initialize, 'v')
HLE_NATIVE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open, 'w')
HLE_NATIVE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer, 'w')
HLE_NATIVE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window, 'w')
HLE_NATIVE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync, 'w')
