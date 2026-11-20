#include "tow2_ge.hpp"
#include "tow2_window.hpp"
#include "ge/ge_renderer.hpp"

#include "psprecomp/common.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace tow2 {
namespace {

constexpr std::uint32_t kGeCommandNop = 0x00u;
constexpr std::uint32_t kGeCommandVertexAddress = 0x01u;
constexpr std::uint32_t kGeCommandIndexAddress = 0x02u;
constexpr std::uint32_t kGeCommandPrimitive = 0x04u;
constexpr std::uint32_t kGeCommandBoundingBox = 0x07u;
constexpr std::uint32_t kGeCommandJump = 0x08u;
constexpr std::uint32_t kGeCommandBoundingBoxJump = 0x09u;
constexpr std::uint32_t kGeCommandCall = 0x0Au;
constexpr std::uint32_t kGeCommandReturn = 0x0Bu;
constexpr std::uint32_t kGeCommandEnd = 0x0Cu;
constexpr std::uint32_t kGeCommandSignal = 0x0Eu;
constexpr std::uint32_t kGeCommandFinish = 0x0Fu;
constexpr std::uint32_t kGeCommandBase = 0x10u;
constexpr std::uint32_t kGeCommandOffsetAddress = 0x13u;
constexpr std::uint32_t kGeCommandOrigin = 0x14u;

struct StackEntry {
    std::uint32_t pc{};
    std::uint32_t offset_address{};
};

struct GeState {
    std::array<std::uint32_t, 256> commands{};
    vcs::GeTransformState transform{};
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    std::uint32_t offset_address{};
    bool bounding_box_visible{true};
    std::vector<StackEntry> stack;
};

GeState g_ge;

struct PendingGeList {
    std::uint32_t id{};
    std::uint32_t list{};
    std::uint32_t stall{};
};

std::deque<PendingGeList> g_pending_ge_lists;
std::uint32_t g_next_ge_list_id = 1u;
bool g_post_osk_trace = false;

std::uint32_t g_framebuffer{};
std::uint32_t g_framebuffer_stride{};
std::uint32_t g_framebuffer_format{};
std::uint64_t g_prim_count{};
std::uint64_t g_pixels_written{};
std::uint64_t g_triangles{};
std::uint64_t g_unsupported_primitives{};
std::uint64_t g_commands_executed{};
std::uint64_t g_lists_executed{};

bool ge_stats() {
    static const bool enabled = std::getenv("PSPRECOMP_GE_STATS") != nullptr;
    return enabled;
}

std::uint32_t ge_guest_address(std::uint32_t physical) {
    // GE addresses are 24-bit physical addresses. RAM lives at 0x08000000;
    // VRAM is already in the 0x04xxxxxx window understood by GuestMemory.
    if (physical < 0x02000000u) return 0x08000000u | physical;
    return physical;
}

std::uint32_t relative_address(std::uint32_t data) {
    const std::uint32_t base_extended =
        ((g_ge.commands[kGeCommandBase] & 0x000F0000u) << 8u) | (data & 0x00FFFFFFu);
    return ge_guest_address((g_ge.offset_address + base_extended) & 0x0FFFFFFFu);
}

bool ge_word_nonzero(psprecomp::GuestMemory &memory, std::uint32_t address) {
    return memory.contains(address, 4u) && memory.load32(address) != 0u;
}

void trace_sublist(psprecomp::GuestMemory &memory, const char *kind,
                   std::uint32_t list, std::uint32_t target) {
    if (std::getenv("PSPRECOMP_TOW2_GE_SUBLIST_TRACE") == nullptr) return;
    static std::uint32_t calls = 0u;
    if (calls++ >= 24u) return;
    std::cerr << "[tow2-ge-sub] " << kind
              << " list=" << psprecomp::hex32(list)
              << " target=" << psprecomp::hex32(target)
              << " zero=" << (ge_word_nonzero(memory, target) ? 0 : 1);
    for (std::uint32_t index = 0u; index < 8u; ++index) {
        const std::uint32_t at = target + index * 4u;
        if (!memory.contains(at, 4u)) break;
        std::cerr << " " << psprecomp::hex32(memory.load32(at));
    }
    std::uint32_t terminator = 0u;
    for (std::uint32_t index = 0u; index < 4096u; ++index) {
        const std::uint32_t at = target + index * 4u;
        if (!memory.contains(at, 4u)) break;
        const std::uint32_t op = memory.load32(at) >> 24u;
        if (op == 0x0Bu || op == 0x0Cu || op == 0x0Fu) {
            terminator = index * 4u;
            break;
        }
    }
    if (terminator != 0u) std::cerr << " term+" << terminator;
    else std::cerr << " term=none";
    std::cerr << "\n";
}

bool run_list(psprecomp::Runtime &runtime, std::uint32_t list, std::uint32_t stall) {
    if (std::getenv("PSPRECOMP_TOW2_GE_DISABLE") != nullptr) return true;
    auto &memory = runtime.memory();
    if (std::getenv("PSPRECOMP_TOW2_GE_SUBLIST_TRACE") != nullptr) {
        static bool saw_nonzero = false;
        if (!saw_nonzero && ge_word_nonzero(memory, 0x084A0000u)) {
            saw_nonzero = true;
            std::cerr << "[tow2-ge-sub] 084A0000 became nonzero at list="
                      << psprecomp::hex32(list) << "\n";
            for (std::uint32_t index = 0u; index < 8u; ++index) {
                const std::uint32_t at = 0x084A0000u + index * 4u;
                if (!memory.contains(at, 4u)) break;
                std::cerr << "  084A+" << index * 4u << "="
                          << psprecomp::hex32(memory.load32(at)) << "\n";
            }
        }
    }
    if (list == 0u || !memory.contains(list, 4u)) return false;

    // Empty command buffers are submitted during startup; they contain no END
    // command and would otherwise look like an infinite NOP stream.
    bool all_zero = true;
    for (std::uint32_t index = 0u; index < 64u && all_zero; ++index) {
        if (!memory.contains(list + index * 4u, 4u) || memory.load32(list + index * 4u) != 0u)
            all_zero = false;
    }
    if (all_zero) return true;

    ++g_lists_executed;
    std::uint32_t pc = list;
    const std::uint32_t start = list;
    bool logged_error = false;
    if (ge_stats()) {
        static bool dumped = false;
        if (!dumped && g_lists_executed >= 3u) {
            dumped = true;
            std::cerr << "[tow2-ge-list] start=" << psprecomp::hex32(start) << "\n";
            for (std::uint32_t index = 0u; index < 32u; ++index) {
                const std::uint32_t at = start + index * 4u;
                if (!memory.contains(at, 4u)) break;
                std::cerr << "  +" << std::setw(4) << index * 4u << " "
                          << psprecomp::hex32(memory.load32(at)) << "\n";
            }
        }
    }

    static const std::uint32_t command_limit = [] {
        const char *text = std::getenv("PSPRECOMP_TOW2_GE_COMMAND_LIMIT");
        return text != nullptr ? static_cast<std::uint32_t>(
            std::strtoul(text, nullptr, 0)) : 100000u;
    }();
    for (std::uint32_t step = 0u; step < command_limit; ++step) {
        ++g_commands_executed;
        if (!memory.contains(pc, 4u)) {
            std::cerr << "[tow2-ge] list " << psprecomp::hex32(start)
                      << " pc " << psprecomp::hex32(pc) << " is outside guest memory\n";
            return false;
        }
        const std::uint32_t op = memory.load32(pc);
        const std::uint32_t op_pc = pc;
        const std::uint32_t command = op >> 24u;
        const std::uint32_t data = op & 0x00FFFFFFu;
        g_ge.commands[command] = op;
        std::uint32_t next_pc = (pc + 4u) & 0x0FFFFFFFu;

        if (command >= 0x2Au && command <= 0x3Fu)
            vcs::update_ge_transform_state(g_ge.transform, command, data);

        switch (command) {
        case kGeCommandNop:
            break;
        case kGeCommandVertexAddress:
            g_ge.vertex_address = relative_address(data);
            break;
        case kGeCommandIndexAddress:
            g_ge.index_address = relative_address(data);
            break;
        case kGeCommandPrimitive: {
            vcs::GeRenderStats stats{};
            std::string error;
            ++g_prim_count;
            if (!vcs::render_ge_primitive(memory, g_ge.commands, g_ge.transform,
                                          g_ge.vertex_address, g_ge.index_address,
                                          data, stats, error, 1u)) {
                if (!logged_error) {
                    logged_error = true;
                    std::cerr << "[tow2-ge] PRIM at " << psprecomp::hex32(op_pc)
                              << " failed: " << error << "\n";
                }
            }
            g_pixels_written += stats.pixels_written;
            g_triangles += stats.triangles;
            g_unsupported_primitives += stats.unsupported_primitives;
            if (ge_stats()) {
                static std::uint32_t logged_prims = 0u;
                if (logged_prims < 8u ||
                    (g_prim_count > 1400u && logged_prims < 24u)) {
                    ++logged_prims;
                    std::cerr << "[tow2-ge-prim] pc=" << psprecomp::hex32(op_pc)
                              << " data=" << psprecomp::hex32(data)
                              << " vaddr=" << psprecomp::hex32(g_ge.vertex_address)
                              << " iaddr=" << psprecomp::hex32(g_ge.index_address)
                              << " vtype=" << psprecomp::hex32(g_ge.commands[0x12u])
                              << " texen=" << psprecomp::hex32(g_ge.commands[0x1Eu])
                              << " texfmt=" << psprecomp::hex32(g_ge.commands[0xC3u])
                              << " texmode=" << psprecomp::hex32(g_ge.commands[0xC2u])
                              << " offset=" << psprecomp::hex32(g_ge.offset_address)
                              << " basecmd=" << psprecomp::hex32(g_ge.commands[0x10u])
                              << " texaddr0=" << psprecomp::hex32(g_ge.commands[0xA0u])
                              << " texbuf0=" << psprecomp::hex32(g_ge.commands[0xA8u])
                              << " texsize0=" << psprecomp::hex32(g_ge.commands[0xB8u])
                              << " clut=" << psprecomp::hex32(g_ge.commands[0xB0u])
                              << " clutfmt=" << psprecomp::hex32(g_ge.commands[0xC5u])
                              << " fbptr=" << psprecomp::hex32(g_ge.commands[0x9Cu])
                              << " fbfmt=" << psprecomp::hex32(g_ge.commands[0xD2u])
                              << " clearmode=" << psprecomp::hex32(g_ge.commands[0xD3u])
                              << " prims=" << stats.primitives
                              << " decoded=" << stats.decoded_vertices
                              << " tris=" << stats.triangles
                              << " pixels=" << stats.pixels_written
                              << " unsupported=" << stats.unsupported_primitives << "\n";
                    const std::uint32_t words_to_dump = g_prim_count > 1400u ? 32u : 8u;
                    for (std::uint32_t word = 0u; word < words_to_dump; ++word) {
                        if (!memory.contains(g_ge.vertex_address + word * 4u, 4u)) break;
                        std::cerr << "  v" << word << "="
                                  << psprecomp::hex32(memory.load32(g_ge.vertex_address + word * 4u));
                    }
                    std::cerr << "\n";
                }
            }
            g_ge.vertex_address = stats.next_vertex_address;
            g_ge.index_address = stats.next_index_address;
            break;
        }
        case kGeCommandBoundingBox: {
            vcs::GeBoundingBoxResult result{};
            std::string error;
            if (vcs::test_ge_bounding_box(memory, g_ge.commands, g_ge.transform,
                                          g_ge.vertex_address, g_ge.index_address,
                                          data & 0xFFFFu, result, error)) {
                g_ge.bounding_box_visible = result.visible;
                g_ge.vertex_address = result.next_vertex_address;
                g_ge.index_address = result.next_index_address;
            }
            break;
        }
        case kGeCommandJump: {
            const std::uint32_t target = relative_address(data & 0x00FFFFFCu);
            trace_sublist(memory, "jump", start, target);
            next_pc = target;
            break;
        }
        case kGeCommandBoundingBoxJump:
            if (!g_ge.bounding_box_visible)
                next_pc = relative_address(data & 0x00FFFFFCu);
            break;
        case kGeCommandCall: {
            const std::uint32_t target = relative_address(data & 0x00FFFFFCu);
            trace_sublist(memory, "call", start, target);
            if (g_ge.stack.size() < 64u) {
                g_ge.stack.push_back(StackEntry{next_pc, g_ge.offset_address});
                next_pc = target;
            }
            break;
        }
        case kGeCommandReturn:
            if (g_ge.stack.empty()) {
                return true;
            } else {
                const StackEntry entry = g_ge.stack.back();
                g_ge.stack.pop_back();
                g_ge.offset_address = entry.offset_address;
                next_pc = entry.pc & 0x0FFFFFFFu;
            }
            break;
        case kGeCommandOffsetAddress:
            g_ge.offset_address = op << 8u;
            break;
        case kGeCommandOrigin:
            g_ge.offset_address = op_pc;
            break;
        case kGeCommandSignal:
            break;
        case kGeCommandEnd:
        case kGeCommandFinish:
            return true;
        default:
            if (std::getenv("PSPRECOMP_TOW2_GE_OPCODE_DIAG") != nullptr &&
                op >= 0x20u && op <= 0x2Fu) {
                static std::uint32_t logged = 0u;
                if (logged++ < 200u) {
                    std::cerr << "[tow2-ge-op] list="
                              << psprecomp::hex32(start)
                              << " op=" << psprecomp::hex32(op)
                              << " data=" << psprecomp::hex32(data) << "\n";
                }
            }
            break;
        }

        pc = next_pc;
        (void)stall;
    }

    std::cerr << "[tow2-ge] list " << psprecomp::hex32(start)
              << " exceeded the command limit\n";
    if (std::getenv("PSPRECOMP_TOW2_GE_BAD_LIST_DUMP") != nullptr) {
        static std::uint32_t dumped = 0u;
        if (dumped < 8u) {
            ++dumped;
            for (std::uint32_t index = 0u; index < 64u; ++index) {
                const std::uint32_t at = start + index * 4u;
                if (!memory.contains(at, 4u)) break;
                std::cerr << "  list+" << index * 4u << "="
                          << psprecomp::hex32(memory.load32(at)) << "\n";
            }
            // The title frame list CALLs this sublist.  It is the usual source
            // of the command-limit warning; dump enough to identify its shape.
            for (std::uint32_t index = 0u; index < 32u; ++index) {
                const std::uint32_t at = 0x084A0000u + index * 4u;
                if (!memory.contains(at, 4u)) break;
                std::cerr << "  sub084A+" << index * 4u << "="
                          << psprecomp::hex32(memory.load32(at)) << "\n";
            }
        }
    }
    return false;
}

} // namespace

void ge_reset() {
    g_ge = GeState{};
    g_pending_ge_lists.clear();
    vcs::reset_ge_transform_state(g_ge.transform);
    g_framebuffer = 0u;
    g_framebuffer_stride = 0u;
    g_framebuffer_format = 0u;
}

void trace_ge_list_meta(psprecomp::GuestMemory &memory, std::uint32_t list,
                        std::uint32_t stall) {
    if (std::getenv("PSPRECOMP_TOW2_GE_STALL_DIAG") == nullptr) return;
    static std::uint32_t logged = 0u;
    if (logged++ >= 24u) return;
    std::cerr << "[tow2-ge-meta] list=" << psprecomp::hex32(list)
              << " stall=" << psprecomp::hex32(stall);
    if (stall != 0u && memory.contains(stall, 4u)) {
        std::cerr << " stall_word=" << psprecomp::hex32(memory.load32(stall));
    }
    std::cerr << " first=";
    std::uint32_t terminator = 0u;
    for (std::uint32_t index = 0u; index < 4096u; ++index) {
        const std::uint32_t at = list + index * 4u;
        if (!memory.contains(at, 4u)) break;
        const std::uint32_t word = memory.load32(at);
        const std::uint32_t op = word >> 24u;
        if (op == 0x0Bu || op == 0x0Cu || op == 0x0Fu) {
            terminator = index * 4u;
            std::cerr << "term+" << terminator << "="
                      << psprecomp::hex32(word);
            break;
        }
    }
    if (terminator == 0u) std::cerr << "none";
    std::cerr << "\n";
}

void execute_ge_list(psprecomp::Runtime &runtime, std::uint32_t list,
                     std::uint32_t stall) {
    const std::uint64_t prims_before = g_prim_count;
    const std::uint64_t pixels_before = g_pixels_written;
    const std::uint64_t triangles_before = g_triangles;
    const std::uint64_t commands_before = g_commands_executed;
    (void)run_list(runtime, list, stall);
    if (ge_stats()) {
        std::cerr << "[tow2-ge-stats] list=" << psprecomp::hex32(list)
                  << " commands=" << (g_commands_executed - commands_before)
                  << " prims=" << (g_prim_count - prims_before)
                  << " triangles=" << (g_triangles - triangles_before)
                  << " pixels=" << (g_pixels_written - pixels_before)
                  << " total_prims=" << g_prim_count
                  << " total_pixels=" << g_pixels_written
                  << " lists=" << g_lists_executed
                  << " unsupported=" << g_unsupported_primitives << "\n";
    }
}

std::uint32_t ge_enqueue(psprecomp::Runtime &runtime, std::uint32_t list,
                         std::uint32_t stall, std::uint32_t callback) {
    (void)callback;
    if (list == 0u) return 0u;
    trace_ge_list_meta(runtime.memory(), list, stall);
    const std::uint32_t id = g_next_ge_list_id++;
    if (g_post_osk_trace) {
        std::cerr << "[tow2-ge-post] enqueue id=" << id
                  << " list=" << psprecomp::hex32(list)
                  << " stall=" << psprecomp::hex32(stall)
                  << " pending=" << g_pending_ge_lists.size() << "\n";
    }
    if (stall != 0u) {
        g_pending_ge_lists.push_back(PendingGeList{id, list, stall});
        return id;
    }
    execute_ge_list(runtime, list, stall);
    return id;
}

std::uint32_t ge_enqueue_head(psprecomp::Runtime &runtime, std::uint32_t list,
                              std::uint32_t stall, std::uint32_t callback) {
    (void)callback;
    if (list == 0u) return 0u;
    trace_ge_list_meta(runtime.memory(), list, stall);
    const std::uint32_t id = g_next_ge_list_id++;
    if (g_post_osk_trace) {
        std::cerr << "[tow2-ge-post] enqueue-head id=" << id
                  << " list=" << psprecomp::hex32(list)
                  << " stall=" << psprecomp::hex32(stall)
                  << " pending=" << g_pending_ge_lists.size() << "\n";
    }
    if (stall != 0u) {
        g_pending_ge_lists.push_front(PendingGeList{id, list, stall});
        return id;
    }
    execute_ge_list(runtime, list, stall);
    return id;
}

void ge_update_stall(std::uint32_t id, std::uint32_t stall) {
    for (PendingGeList &pending : g_pending_ge_lists) {
        if (pending.id == id) {
            pending.stall = stall;
            break;
        }
    }
}

void ge_sync(psprecomp::Runtime &runtime, std::uint32_t id) {
    if (g_post_osk_trace) {
        std::cerr << "[tow2-ge-post] sync id=" << id
                  << " pending=" << g_pending_ge_lists.size() << "\n";
    }
    for (auto iterator = g_pending_ge_lists.begin();
         iterator != g_pending_ge_lists.end();) {
        if (id == 0u || iterator->id == id) {
            execute_ge_list(runtime, iterator->list, iterator->stall);
            iterator = g_pending_ge_lists.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void ge_draw_sync(psprecomp::Runtime &runtime) {
    if (g_post_osk_trace && !g_pending_ge_lists.empty()) {
        std::cerr << "[tow2-ge-post] draw-sync pending="
                  << g_pending_ge_lists.size() << "\n";
    }
    while (!g_pending_ge_lists.empty()) {
        const PendingGeList pending = g_pending_ge_lists.front();
        g_pending_ge_lists.pop_front();
        execute_ge_list(runtime, pending.list, pending.stall);
    }
}

void ge_set_post_osk(bool active) {
    g_post_osk_trace = active;
    if (active) {
        std::cerr << "[tow2-ge-post] tracing enabled\n";
    }
}

void ge_set_framebuffer(psprecomp::Runtime &runtime, std::uint32_t address,
                        std::uint32_t stride, std::uint32_t format) {
    // Some guest paths enqueue with a stall address and present without an
    // explicit list sync.  Draw any remaining queued work before the present.
    ge_draw_sync(runtime);
    g_framebuffer = address;
    g_framebuffer_stride = stride;
    g_framebuffer_format = format;
    window_present(runtime.memory(), address, stride, format);
}

void ge_shutdown() {
    window_shutdown();
}

} // namespace tow2
