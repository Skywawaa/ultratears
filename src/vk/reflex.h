// NVIDIA Reflex (VK_NV_low_latency2), on by default where the driver offers it; TOTK_REFLEX=0 off, =2 with boost.
//  - game thread: vkr_sleep at the start of a frame (after the frame limiter), markers for the end of the simulation
//    and the start of its rendering;
//  - replay thread: the end of the frame's GPU submission;
//  - present thread: the present (with that frame's id as its VkPresentIdKHR).
#pragma once
#include <stdint.h>

#include "vk.h"

int vkr_on(void);
const void *vkr_swapchain_info(const void *next);   // pNext chain for vkCreateSwapchainKHR
void vkr_swapchain(VkSwapchainKHR swapchain);       // after creating one (low-latency mode), VK_NULL_HANDLE before destroying it
void vkr_sleep(uint64_t frame);                     // waits until the driver says to start the frame, then SIMULATION_START
void vkr_sim_end(uint64_t frame);                   // the game presented: SIMULATION_END and RENDERSUBMIT_START
void vkr_marker(VkLatencyMarkerNV marker, uint64_t frame);
