#pragma once

#include "psprecomp/runtime.hpp"

namespace tow1 {

// Registers the title's HLE imports. Intentionally minimal: an import that has
// not been implemented stops the runtime with its library::NID name, which is
// the boot-triage signal. Do not add blanket "return 0" handlers here.
//
// user_arena_start is the 256-byte-aligned end of the loaded image. It is the
// floor for the upward-growing partition arena and the downward-growing thread
// stacks.
void register_hle(psprecomp::Runtime &runtime, std::uint32_t user_arena_start);

// Diagnostic: prints every live thread and its scheduler state.
void report_threads();

} // namespace tow1
