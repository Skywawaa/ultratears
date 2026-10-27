// NVIDIA Reflex through VK_NV_low_latency2 (see reflex.h). The driver's low-latency mode keeps the GPU's queue short:
// the game thread sleeps at the start of each frame (vkLatencySleepNV) until the driver says to begin, so input is read
// as late as possible. Frame ids are the game's frame numbers (hle_nvn.c g_frame + 1 while a frame is simulated), the
// same ids the window's presents carry (VkPresentIdKHR), so the driver can match the markers of one frame.
#include "reflex.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;   // the swapchain may be rebuilt while the game thread sleeps
static VkSwapchainKHR sc;
static VkSemaphore sem;
static uint64_t sem_value;
static int mode = -1;   // TOTK_REFLEX: 0 off, 1 on (default), 2 on with boost (the GPU clocks stay high)

int vkr_on(void) {
    if (mode < 0) {
        const char *e = getenv("TOTK_REFLEX");
        mode = e && *e ? atoi(e) : 1;
        if (mode && !vkc.has_low_latency) { LOG("Reflex: not available (VK_NV_low_latency2 and VK_KHR_present_id needed)"); mode = 0; }
        else LOG("Reflex %s (TOTK_REFLEX)", mode == 2 ? "on + boost" : mode ? "on" : "off");
    }
    return mode > 0;
}

const void *vkr_swapchain_info(const void *next) {
    static VkSwapchainLatencyCreateInfoNV li = {VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV, .latencyModeEnable = VK_TRUE};
    if (!vkr_on()) return next;
    li.pNext = next;
    return &li;
}

void vkr_swapchain(VkSwapchainKHR swapchain) {
    if (!vkr_on()) return;
    pthread_mutex_lock(&mu);
    sc = swapchain;
    if (sc) {
        VkLatencySleepModeInfoNV m = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV, .lowLatencyMode = VK_TRUE,
                                      .lowLatencyBoost = mode == 2, .minimumIntervalUs = 0};
        vk_check(vk.vkSetLatencySleepModeNV(vkc.device, sc, &m), "vkSetLatencySleepModeNV");
    }
    pthread_mutex_unlock(&mu);
}

void vkr_sleep(uint64_t frame) {
    if (!vkr_on()) return;
    pthread_mutex_lock(&mu);
    if (!sem) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        if (!vk_check(vk.vkCreateSemaphore(vkc.device, &si, NULL, &sem), "Reflex semaphore")) { mode = 0; pthread_mutex_unlock(&mu); return; }
    }
    uint64_t v = 0;
    if (sc) {
        VkLatencySleepInfoNV li = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, .signalSemaphore = sem, .value = ++sem_value};
        if (vk.vkLatencySleepNV(vkc.device, sc, &li) == VK_SUCCESS) v = sem_value;
    }
    pthread_mutex_unlock(&mu);
    if (v) {   // at most 100 ms: a driver that stops signalling never holds the game
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &v};
        vk.vkWaitSemaphores(vkc.device, &wi, 100000000ull);
    }
    vkr_marker(VK_LATENCY_MARKER_SIMULATION_START_NV, frame);
}

void vkr_sim_end(uint64_t frame) {
    vkr_marker(VK_LATENCY_MARKER_SIMULATION_END_NV, frame);
    vkr_marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, frame);
}

void vkr_marker(VkLatencyMarkerNV marker, uint64_t frame) {
    if (!vkr_on() || !frame) return;
    pthread_mutex_lock(&mu);
    if (sc) {
        VkSetLatencyMarkerInfoNV mi = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = frame, .marker = marker};
        vk.vkSetLatencyMarkerNV(vkc.device, sc, &mi);
    }
    pthread_mutex_unlock(&mu);
}
