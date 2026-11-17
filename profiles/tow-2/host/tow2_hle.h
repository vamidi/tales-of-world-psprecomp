#pragma once

#include "psprecomp/runtime.hpp"

namespace tow2 {

// Phase B scaffold. This is intentionally empty: an import with no registered
// contract stops the runtime with "Missing HLE import <library>::<nid>", which
// is the boot-triage signal. Add contracts here as the entry path demands them;
// do not add blanket "return 0" handlers.
//
// user_arena_start is the 256-byte-aligned end of the loaded image. It becomes
// the floor for the upward-growing partition arena and the downward-growing
// thread stacks once the scheduler HLE exists.
void register_hle(psprecomp::Runtime &runtime, std::uint32_t user_arena_start);

// Diagnostic: prints every live thread and its scheduler state once a
// scheduler exists. No-op during Phase B.
void report_threads();

} // namespace tow2
