#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace tow1 {

// Queues a guest GE display list.  A non-zero stall address means the guest is
// still filling the command buffer; execution is deferred until a sync call.
std::uint32_t ge_enqueue(psprecomp::Runtime &runtime, std::uint32_t list,
                         std::uint32_t stall, std::uint32_t callback);
std::uint32_t ge_enqueue_head(psprecomp::Runtime &runtime, std::uint32_t list,
                              std::uint32_t stall, std::uint32_t callback);
void ge_update_stall(std::uint32_t id, std::uint32_t stall);
void ge_sync(psprecomp::Runtime &runtime, std::uint32_t id);
void ge_draw_sync(psprecomp::Runtime &runtime);

// Records the currently displayed framebuffer and presents it.
void ge_set_framebuffer(psprecomp::Runtime &runtime, std::uint32_t address,
                        std::uint32_t stride, std::uint32_t format);

void ge_reset();
void ge_shutdown();

} // namespace tow1
