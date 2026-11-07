#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>

namespace tow1 {

void window_start();
void window_present(const psprecomp::GuestMemory &memory, std::uint32_t address,
                    std::uint32_t stride, std::uint32_t format);
void window_shutdown();

// Live PSP button mask sampled from the host window, or 0 when the window is
// unavailable. Bit assignments match sceCtrl's SceCtrlData buttons.
std::uint32_t window_buttons();

} // namespace tow1
