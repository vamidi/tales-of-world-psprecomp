#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>

namespace tow1 {

void window_start();
void window_present(const psprecomp::GuestMemory &memory, std::uint32_t address,
                    std::uint32_t stride, std::uint32_t format);

// Arms a one-shot dump of the next presented frame.  Used to capture the first
// frame after an asynchronous guest transition (for example OSK completion).
void window_arm_post_osk_capture(const char *path);

// Schedules a button mask relative to the current present count.
void window_schedule_post_osk_input(std::uint32_t mask, std::uint32_t delay_frames,
                                    std::uint32_t hold_frames);
void window_mark_post_osk();
std::uint32_t window_post_osk_present_count();
void window_shutdown();

// Live PSP button mask sampled from the host window, or 0 when the window is
// unavailable. Bit assignments match sceCtrl's SceCtrlData buttons.
std::uint32_t window_buttons();

} // namespace tow1
