#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace tow1 {

// Executes one guest GE display list synchronously. The rasterizer writes into
// guest VRAM, so the display framebuffer can be presented from GuestMemory.
void ge_enqueue(psprecomp::Runtime &runtime, std::uint32_t list,
                std::uint32_t stall, std::uint32_t callback);

// Records the currently displayed framebuffer and presents it.
void ge_set_framebuffer(psprecomp::Runtime &runtime, std::uint32_t address,
                        std::uint32_t stride, std::uint32_t format);

void ge_reset();
void ge_shutdown();

} // namespace tow1
