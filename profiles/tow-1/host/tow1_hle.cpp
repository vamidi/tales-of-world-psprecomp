#include "tow1_hle.h"
#include "tow1_atrac.hpp"
#include "tow1_ge.hpp"
#include "tow1_mpeg.hpp"
#include "tow1_window.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tow1 {
namespace {

constexpr std::uint32_t kUserRamTop = 0x0A000000u;
constexpr std::uint32_t kModuleStackSize = 0x10000u;
constexpr std::uint32_t kThreadReturnPc = 0x00000000u;

enum class ThreadState {
    Created,
    Ready,
    Running,
    Delayed,
    Waiting,
    Sleeping,
    Completed,
};

struct ThreadRecord {
    std::string name;
    std::uint32_t entry{};
    std::uint32_t priority{};
    std::uint32_t stack_size{};
    std::uint32_t attributes{};
    std::uint32_t stack_top{};
    std::uint32_t stack_bottom{};
    std::uint32_t kernel_context{};
    ThreadState state{ThreadState::Created};
    std::uint32_t exit_status{};
    psprecomp::AllegrexContext context{};
    std::uint64_t delay_until_us{};
    std::uint64_t delay_sequence{};
};

struct ReadyEntry {
    std::int32_t uid{};
    std::uint64_t sequence{};
};

struct ThreadWaiter {
    std::int32_t uid{};
    std::uint32_t status_pointer{};
    psprecomp::AllegrexContext context{};
};

struct Scheduler {
    std::unordered_map<std::int32_t, ThreadRecord> threads;
    std::vector<ReadyEntry> ready;
    std::unordered_map<std::int32_t, std::vector<ThreadWaiter>> end_waiters;
    std::int32_t current_uid{0};
    std::int32_t next_uid{1};
    std::uint32_t next_stack_top{kUserRamTop - kModuleStackSize};
    std::uint64_t now_us{0};
    std::uint64_t next_sequence{1};
};

Scheduler g_scheduler;

struct PartitionBlock {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
};

struct PartitionTable {
    std::int32_t next_uid{0x100};
    std::uint32_t next_address{};
    std::unordered_map<std::int32_t, PartitionBlock> blocks;
};

PartitionTable g_partitions;

struct SemaphoreWaiter {
    std::int32_t uid{};
    std::int32_t count{};
    psprecomp::AllegrexContext context{};
};

struct SemaphoreRecord {
    std::string name;
    std::int32_t value{};
    std::int32_t maximum{};
    std::vector<SemaphoreWaiter> waiters;
};

std::unordered_map<std::int32_t, SemaphoreRecord> g_semaphores;
std::int32_t g_next_semaphore_uid = 0x200;

// The game's main loop waits on a semaphore named "SleepSema". With
// PSPRECOMP_TOW1_VBLANK_SEMA set, the scheduler tick models a vblank signal
// for it so we can test whether the idle wait is frame-driven.
std::int32_t g_sleep_semaphore_uid = -1;
std::uint64_t g_next_vblank_us = 16683u;

struct CallbackRecord {
    std::string name;
    std::uint32_t function{};
    std::uint32_t argument{};
    std::int32_t owner_uid{};
};

std::unordered_map<std::int32_t, CallbackRecord> g_callbacks;
std::int32_t g_next_callback_uid = 0x300;

struct MutexWaiter {
    std::int32_t uid{};
    std::int32_t count{};
    psprecomp::AllegrexContext context{};
};

struct MutexRecord {
    std::string name;
    std::int32_t owner{-1};
    std::int32_t count{};
    std::vector<MutexWaiter> waiters;
};

std::unordered_map<std::int32_t, MutexRecord> g_mutexes;
std::int32_t g_next_mutex_uid = 0x400;

struct EventFlagWaiter {
    std::int32_t uid{};
    std::uint32_t bits{};
    std::uint32_t mode{};
    std::uint32_t output_pointer{};
    psprecomp::AllegrexContext context{};
};

struct EventFlagRecord {
    std::string name;
    std::uint32_t pattern{};
    std::vector<EventFlagWaiter> waiters;
};

std::unordered_map<std::int32_t, EventFlagRecord> g_event_flags;
std::int32_t g_next_event_flag_uid = 0x500;

struct MbxWaiter {
    std::int32_t uid{};
    std::uint32_t output_pointer{};
    psprecomp::AllegrexContext context{};
};

struct MbxRecord {
    std::string name;
    std::deque<std::uint32_t> messages;
    std::vector<MbxWaiter> waiters;
};

std::unordered_map<std::int32_t, MbxRecord> g_mbxs;
std::int32_t g_next_mbx_uid = 0x600;

bool g_volatile_memory_locked = false;

std::array<bool, 8> g_audio_channel_reserved{};
std::array<std::uint32_t, 8> g_audio_channel_samples{};

struct SasState {
    std::uint32_t core{};
    std::int32_t grain{};
    std::int32_t max_voices{};
    std::int32_t output_mode{};
    std::int32_t sample_rate{};
};
SasState g_sas;

struct FileHandle {
    std::ifstream stream;
    std::uint64_t size{};
    std::uint64_t position{};
    std::string psp_path;
    std::int64_t async_result{0};
    bool writable{};
    bool device{};
};

std::unordered_map<std::int32_t, FileHandle> g_files;
std::int32_t g_next_fd = 3;
std::uint64_t g_read_diag_count = 0;
std::uint64_t g_lseek_diag_count = 0;

struct DirHandle {
    std::vector<std::filesystem::directory_entry> entries;
    std::size_t next{};
};

std::unordered_map<std::int32_t, DirHandle> g_dirs;
std::int32_t g_next_dir_fd = 0x100;

struct LoadedModule {
    std::string path;
};
std::unordered_map<std::int32_t, LoadedModule> g_loaded_modules;
std::int32_t g_next_module_uid = 0x400;
std::string g_pmf_host_path;

bool file_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_FILE_DIAG") != nullptr;
    return enabled;
}

bool module_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_MODULE_DIAG") != nullptr;
    return enabled;
}

bool mpeg_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_TOW1_MPEG_DIAG") != nullptr;
    return enabled;
}

std::uint32_t g_display_mode = 0u;
std::uint32_t g_display_width = 480u;
std::uint32_t g_display_height = 272u;
std::int32_t g_savedata_status = 0;

struct MpegState {
    std::uint32_t address{};
    std::uint32_t work{};
    std::uint32_t ringbuffer{};
    std::uint32_t frame_width{};
    std::uint32_t stream_offset{};
    std::uint32_t stream_size{};
    std::uint32_t video_frames{};
    std::uint32_t last_status{};
    std::uint32_t pixel_mode{3u};
    std::uint32_t es_buffer{};
};
std::unordered_map<std::uint32_t, MpegState> g_mpeg_states;
std::unique_ptr<tow1::MpegPlayer> g_mpeg_player;
std::unique_ptr<tow1::AtracPlayer> g_atrac_player;
std::vector<std::uint8_t> g_mpeg_frame;
bool g_mpeg_video_eof = false;

std::uint32_t byte_swap32(std::uint32_t value) {
    return ((value & 0x000000FFu) << 24u) |
           ((value & 0x0000FF00u) << 8u) |
           ((value & 0x00FF0000u) >> 8u) |
           ((value & 0xFF000000u) >> 24u);
}

MpegState *mpeg_state_for(psprecomp::Runtime &runtime, std::uint32_t address) {
    if (address == 0u || !runtime.memory().contains(address, 4u)) return nullptr;
    const std::uint32_t handle = runtime.memory().load32(address);
    if (auto found = g_mpeg_states.find(handle); found != g_mpeg_states.end())
        return &found->second;
    if (auto found = g_mpeg_states.find(address); found != g_mpeg_states.end())
        return &found->second;
    return nullptr;
}

bool ensure_atrac_player(psprecomp::Runtime &runtime, std::string &error) {
    if (g_atrac_player && g_atrac_player->is_open()) return true;
    std::string path = g_pmf_host_path;
    if (path.empty()) {
        path = (runtime.game_root() / "PSP_GAME" / "USRDIR" / "title" / "title.pmf").string();
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = "Atrac source not found: " + path;
        return false;
    }
    auto player = std::make_unique<tow1::AtracPlayer>();
    if (!player->open(path, error)) return false;
    g_atrac_player = std::move(player);
    if (mpeg_diag())
        std::cerr << "[tow1-mpeg] opened Atrac stream, frames="
                  << g_atrac_player->decoded_frames() << "\n";
    return true;
}

bool ensure_mpeg_player(psprecomp::Runtime &runtime, std::string &error) {
    if (g_mpeg_player && g_mpeg_player->is_open()) return true;
    std::string path = g_pmf_host_path;
    if (path.empty()) {
        path = (runtime.game_root() / "PSP_GAME" / "USRDIR" / "title" / "title.pmf").string();
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = "PMF source not found: " + path;
        return false;
    }
    auto player = std::make_unique<tow1::MpegPlayer>();
    if (!player->open(path, error)) return false;
    g_mpeg_player = std::move(player);
    if (mpeg_diag()) std::cerr << "[tow1-mpeg] opened host stream \"" << path << "\"\n";
    return true;
}

bool write_mpeg_frame(psprecomp::Runtime &runtime, std::uint32_t destination,
                      std::uint32_t frame_width, std::uint32_t pixel_mode,
                      const std::vector<std::uint8_t> &rgba, int width, int height) {
    if (destination == 0u || width <= 0 || height <= 0 ||
        frame_width == 0u || frame_width > 2048u) return false;
    const std::uint32_t copy_width = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(width), frame_width);
    const std::uint32_t bytes_per_pixel = pixel_mode == 3u ? 4u : 2u;
    const std::uint64_t required = static_cast<std::uint64_t>(frame_width) *
                                   static_cast<std::uint64_t>(height) * bytes_per_pixel;
    if (required > 0x10000000u ||
        !runtime.memory().contains(destination, static_cast<std::size_t>(required))) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        const std::uint8_t *source = rgba.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        const std::uint32_t row = destination + static_cast<std::uint32_t>(y) *
            frame_width * bytes_per_pixel;
        for (std::uint32_t x = 0; x < copy_width; ++x) {
            const std::uint8_t r = source[x * 4u + 0u];
            const std::uint8_t g = source[x * 4u + 1u];
            const std::uint8_t b = source[x * 4u + 2u];
            if (pixel_mode == 3u) {
                runtime.memory().store32(row + x * 4u,
                    static_cast<std::uint32_t>(r) |
                    (static_cast<std::uint32_t>(g) << 8u) |
                    (static_cast<std::uint32_t>(b) << 16u));
            } else if (pixel_mode == 0u) {
                runtime.memory().store16(row + x * 2u,
                    static_cast<std::uint16_t>(((r >> 3u) << 11u) |
                                               ((g >> 2u) << 5u) |
                                               (b >> 3u)));
            } else if (pixel_mode == 1u) {
                runtime.memory().store16(row + x * 2u,
                    static_cast<std::uint16_t>(((b >> 3u) << 11u) |
                                               ((g >> 3u) << 6u) |
                                               ((r >> 3u) << 1u) | 1u));
            } else if (pixel_mode == 2u) {
                runtime.memory().store16(row + x * 2u,
                    static_cast<std::uint16_t>(((b >> 4u) << 12u) |
                                               ((g >> 4u) << 8u) |
                                               ((r >> 4u) << 4u)));
            } else {
                return false;
            }
        }
    }
    return true;
}
std::uint32_t g_framebuffer = 0u;
std::uint32_t g_framebuffer_stride = 0u;
std::uint32_t g_framebuffer_format = 0u;

// Main module identity. The PRX is the only loaded module for now; the game
// queries this to find which module owns an address.
constexpr std::uint32_t kMainModuleUid = 1u;
std::uint32_t g_module_end = psprecomp::kDefaultPspUserLoadBase;

bool thread_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_THREAD_DIAG") != nullptr;
    return enabled;
}

bool heap_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_TOW1_HEAP_DIAG") != nullptr;
    return enabled;
}

// Resolved from generated_unit_0397.cpp:
//   lui  s1, 0x08E1
//   lw   a0, -1952(s1)   ; 0x08E0F860  MemoryMutex
//   addiu a0, s1, -1984  ; 0x08E0F840  heap descriptor
constexpr std::uint32_t kGameHeap = 0x08E0F840u;
constexpr std::uint32_t kGameHeapMutex = 0x08E0F860u;
constexpr std::uint32_t kGameHeapAlloc = 0x08B254D8u;
constexpr std::uint32_t kGameHeapSplit = 0x08B255F4u;
constexpr std::uint32_t kArchiveFileSize = 0x08AFB130u;
constexpr std::uint32_t kArchiveFileCopy = 0x08AFB3B8u;
constexpr std::uint32_t kArchiveFileOpen = 0x08B062F8u;
constexpr std::uint32_t kNewlibGetReent = 0x08B6E440u;
constexpr std::uint32_t kNewlibReentTable = 0x08E18510u;
constexpr std::uint32_t kNewlibImpurePtr = 0x08DB24E8u;
std::uint32_t g_heap_diag_calls = 0u;
std::uint32_t g_heap_diag_last_heap = 0u;
std::uint32_t g_heap_diag_last_size = 0u;
std::uint32_t g_last_archive_open_path = 0u;
std::uint32_t g_ge_diag_lists = 0u;
std::uint32_t g_ge_diag_prim_lists = 0u;
std::uint32_t g_last_ppt_object = 0u;
std::uint64_t g_avc_decode_calls = 0u;
std::uint64_t g_vblank_wait_generation = 0u;
std::uint64_t g_vblank_tick_generation = 0u;

bool ge_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_GE_DIAG") != nullptr;
    return enabled;
}

// Diagnostic host-side upload for the title PPT assets.  The guest PPT loader
// leaves the BNG texture/palette buffers in their initialized (0xFF/black)
// state; this reconstructs the CLUT8 surface directly from the decompressed
// title archive so the title background can be inspected independently of the
// guest loader.
bool upload_ppt_data(psprecomp::Runtime &rt, std::uint32_t base, const char *which) {
    auto &memory = rt.memory();
    if (!memory.contains(base, 0x20230u)) return false;
    const std::uint8_t *src = memory.raw_pointer(base + 0x20u, 0x1FE00u);
    const std::uint8_t *pal = memory.raw_pointer(base + 0x1FE30u, 0x400u);
    auto *texture = memory.raw_pointer(0x08FD45C0u, 0x40000u);
    auto *clut = memory.raw_pointer(0x08FF4400u, 0x400u);
    if (src == nullptr || pal == nullptr || texture == nullptr || clut == nullptr) return false;
    constexpr std::uint32_t tex_width = 480u;
    constexpr std::uint32_t tex_height = 272u;
    constexpr std::uint32_t blocks_per_row = (tex_width + 15u) / 16u;
    for (std::uint32_t y = 0u; y < tex_height; ++y) {
        for (std::uint32_t x = 0u; x < tex_width; ++x) {
            const std::uint32_t offset =
                ((y / 8u) * blocks_per_row + x / 16u) * 128u +
                (y & 7u) * 16u + (x & 15u);
            texture[offset] = src[y * tex_width + x];
        }
    }
    std::memcpy(clut, pal, 0x400u);
    if (std::getenv("PSPRECOMP_TOW1_PPT_SOLID") != nullptr) {
        std::memset(texture, 1, 0x40000u);
        for (std::uint32_t i = 0u; i < 0x100u; ++i)
            std::memcpy(clut + i * 4u, "\xFF\x00\x00\xFF", 4u);
        std::cerr << "[tow1-ppt-upload] SOLID red fill\n";
    }
    std::cerr << "[tow1-ppt-upload] uploaded " << which
              << " base=" << psprecomp::hex32(base)
              << " texture=0x08FD45C0 clut=0x08FF4400\n";
    return true;
}

bool upload_title_ppt(psprecomp::Runtime &rt, std::uint32_t object, const char *which) {
    auto &memory = rt.memory();
    if (!memory.contains(object + 0x24u, 4u)) return false;
    const std::uint32_t width = memory.load32(object + 0x10u);
    const std::uint32_t height = memory.load32(object + 0x14u);
    std::cerr << "[tow1-ppt-upload] probe object=" << psprecomp::hex32(object)
              << " size=" << width << "x" << height << "\n";
    if (width != 480u || height != 270u) return false;
    const std::uint32_t cfile = memory.load32(object + 0x1Cu);
    if (!memory.contains(cfile + 0x90u, 4u)) return false;
    std::string cfile_path;
    if (memory.contains(cfile + 0x8Cu, 1u))
        cfile_path = memory.read_c_string(cfile + 0x8Cu, 160u);
    std::cerr << "[tow1-ppt-upload] candidate object=" << psprecomp::hex32(object)
              << " size=" << width << "x" << height
              << " cfile=" << psprecomp::hex32(cfile)
              << " path=\"" << cfile_path << "\"\n";
    if (cfile_path.find("title.arc") == std::string::npos) return false;
    const std::uint32_t archive = memory.load32(cfile + 0xCu);
    const std::uint32_t base = archive + (height == 270u ? 0x200u : 0x43000u);
    if (!memory.contains(base, 0x20230u)) {
        std::cerr << "[tow1-ppt-upload] missing archive base=" << psprecomp::hex32(archive)
                  << " cfile=" << psprecomp::hex32(cfile) << "\n";
        return false;
    }
    return upload_ppt_data(rt, base, which);
}

void dump_ge_list(psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t stall,
                  std::uint32_t callback, const char *kind) {
    if (!ge_diag() || address == 0u || g_ge_diag_lists >= 40u) return;
    ++g_ge_diag_lists;
    const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    std::array<std::uint32_t, 512> words{};
    std::uint32_t count = 0u;
    bool has_prim = false;
    bool all_zero = true;
    for (; count < words.size(); ++count) {
        const std::uint32_t at = address + count * 4u;
        if (!rt.memory().contains(at, 4u)) break;
        words[count] = rt.memory().load32(at);
        if (words[count] != 0u) all_zero = false;
        const std::uint32_t opcode = words[count] >> 24u;
        if (opcode == 0x04u) has_prim = true;
        if (opcode == 0x0Bu || opcode == 0x0Cu) {
            ++count;
            break;
        }
    }
    std::cerr << "[tow1-ge] " << kind << " list=" << psprecomp::hex32(address)
              << " stall=" << psprecomp::hex32(stall)
              << " cb=" << psprecomp::hex32(callback)
              << " words=" << count
              << " prim=" << (has_prim ? "yes" : "no")
              << " zero=" << (all_zero ? "yes" : "no")
              << " uid=" << g_scheduler.current_uid
              << " name=\"" << (current != g_scheduler.threads.end() ? current->second.name : "?")
              << "\"\n";
    if ((!has_prim && g_ge_diag_lists != 1u) || g_ge_diag_prim_lists >= 4u) return;
    if (has_prim) ++g_ge_diag_prim_lists;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const std::uint32_t word = words[index];
        const std::uint32_t opcode = word >> 24u;
        const std::uint32_t argument = word & 0x00FFFFFFu;
        std::cerr << "[tow1-ge]   +" << std::setw(4) << index * 4u
                  << " " << psprecomp::hex32(word)
                  << " op=" << std::setw(2) << opcode
                  << " arg=" << psprecomp::hex32(argument) << "\n";
    }
}

void dump_heap(psprecomp::Runtime &rt, const char *phase, std::uint32_t heap,
               std::uint32_t request) {
    auto &memory = rt.memory();
    const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    std::cerr << "[tow1-heap] " << phase
              << " uid=" << g_scheduler.current_uid
              << " name=\"" << (current != g_scheduler.threads.end() ? current->second.name : "?")
              << "\" heap=" << psprecomp::hex32(heap)
              << " request=" << request;
    if (heap == 0u || !memory.contains(heap, 0x20u)) {
        std::cerr << " (invalid descriptor)\n";
        return;
    }
    const std::uint32_t first = memory.load32(heap + 8u);
    std::cerr << " first=" << psprecomp::hex32(first)
              << " limit=" << psprecomp::hex32(memory.load32(heap + 0x10u))
              << " align=" << psprecomp::hex32(memory.load32(heap + 0x18u))
              << " mask=" << psprecomp::hex32(memory.load32(heap + 0x1Cu))
              << " mutex=" << psprecomp::hex32(memory.load32(kGameHeapMutex))
              << "\n";

    std::uint32_t block = first;
    for (std::uint32_t index = 0u; block != 0u && index < 32u; ++index) {
        if (!memory.contains(block, 16u)) {
            std::cerr << "[tow1-heap]   block=" << psprecomp::hex32(block)
                      << " (outside guest memory)\n";
            break;
        }
        const std::uint32_t size = memory.load32(block);
        const std::uint32_t end = memory.load32(block + 4u);
        const std::uint32_t prev = memory.load32(block + 8u);
        const std::uint32_t next = memory.load32(block + 12u);
        std::cerr << "[tow1-heap]   [" << index << "] block=" << psprecomp::hex32(block)
                  << " size=" << psprecomp::hex32(size)
                  << " end=" << psprecomp::hex32(end)
                  << " prev=" << psprecomp::hex32(prev)
                  << " next=" << psprecomp::hex32(next) << "\n";
        if (next == block) break;
        block = next;
    }
}

void dump_file_object(psprecomp::Runtime &rt, std::uint32_t reader) {
    auto &memory = rt.memory();
    if (reader == 0u || !memory.contains(reader, 8u)) {
        std::cerr << "[tow1-archive] reader=" << psprecomp::hex32(reader)
                  << " (invalid)\n";
        return;
    }
    const std::uint32_t file = memory.load32(reader + 4u);
    if (file == 0u || !memory.contains(file, 0x20u)) {
        std::cerr << "[tow1-archive] reader=" << psprecomp::hex32(reader)
                  << " file=" << psprecomp::hex32(file) << " (invalid)\n";
        return;
    }
    const std::uint32_t vtable = memory.load32(file);
    std::cerr << "[tow1-archive] reader=" << psprecomp::hex32(reader)
              << " file=" << psprecomp::hex32(file)
              << " vtable=" << psprecomp::hex32(vtable) << "\n";
    if (memory.contains(vtable, 32u)) {
        std::cerr << "[tow1-archive]   vtable:";
        for (std::uint32_t index = 0u; index < 8u; ++index)
            std::cerr << " " << psprecomp::hex32(memory.load32(vtable + index * 4u));
        std::cerr << "\n";
    }
    for (std::uint32_t offset = 0u; offset < 0x20u; offset += 16u) {
        std::cerr << "[tow1-archive]   file+" << psprecomp::hex32(offset) << ":";
        for (std::uint32_t word = 0u; word < 16u; word += 4u)
            std::cerr << " " << psprecomp::hex32(memory.load32(file + offset + word));
        std::cerr << "\n";
    }
    if (memory.contains(file + 0xF0u, 0x40u)) {
        for (std::uint32_t offset = 0xF0u; offset < 0x130u; offset += 16u) {
            std::cerr << "[tow1-archive]   file+" << psprecomp::hex32(offset) << ":";
            for (std::uint32_t word = 0u; word < 16u; word += 4u)
                std::cerr << " " << psprecomp::hex32(memory.load32(file + offset + word));
            std::cerr << "\n";
        }
    }
}

void heap_pre_chained(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
                      std::uint32_t target_pc, std::uint32_t) {
    if (!heap_diag()) return;
    if (target_pc == kArchiveFileCopy || target_pc == kArchiveFileSize) {
        const char *kind = target_pc == kArchiveFileCopy ? "copy" : "size";
        std::cerr << "[tow1-archive] " << kind
                  << " reader=" << psprecomp::hex32(ctx.gpr[4])
                  << " dest=" << psprecomp::hex32(ctx.gpr[5]) << "\n";
        dump_file_object(rt, ctx.gpr[4]);
        return;
    }
    if (target_pc == kNewlibGetReent) {
        auto &memory = rt.memory();
        const std::uint32_t k0 = ctx.gpr[26];
        const std::uint32_t module_info =
            k0 != 0u && memory.contains(k0 + 4u, 4u) ? memory.load32(k0 + 4u) : 0u;
        const std::uint32_t module_id =
            module_info != 0u && memory.contains(module_info + 0x37Cu, 4u)
                ? memory.load32(module_info + 0x37Cu) : 0u;
        const std::uint32_t table_head =
            memory.contains(kNewlibReentTable, 4u)
                ? memory.load32(kNewlibReentTable) : 0u;
        std::cerr << "[tow1-reent] k0=" << psprecomp::hex32(k0)
                  << " module_info=" << psprecomp::hex32(module_info)
                  << " module_id=" << psprecomp::hex32(module_id)
                  << " table=" << psprecomp::hex32(table_head)
                  << " impure=" << psprecomp::hex32(
                         memory.contains(kNewlibImpurePtr, 4u)
                             ? memory.load32(kNewlibImpurePtr) : 0u)
                  << "\n";
        return;
    }
    if (target_pc == kArchiveFileOpen) {
        g_last_archive_open_path = ctx.gpr[5];
        std::string path;
        if (g_last_archive_open_path != 0u &&
            rt.memory().contains(g_last_archive_open_path, 1u))
            path = rt.memory().read_c_string(g_last_archive_open_path, 256u);
        std::cerr << "[tow1-archive] open path="
                  << psprecomp::hex32(g_last_archive_open_path)
                  << " \"" << path << "\"\n";
        return;
    }
    if (target_pc != kGameHeapAlloc) return;
    ++g_heap_diag_calls;
    g_heap_diag_last_heap = ctx.gpr[4];
    g_heap_diag_last_size = ctx.gpr[5];
    if (g_heap_diag_calls <= 64u || ctx.gpr[5] == 196u)
        dump_heap(rt, "alloc", ctx.gpr[4], ctx.gpr[5]);
}

void heap_post_chained(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
                       std::uint32_t target_pc, std::uint32_t) {
    if (!heap_diag()) return;
    if (target_pc == kArchiveFileSize) {
        std::cerr << "[tow1-archive] size result=" << psprecomp::hex32(ctx.gpr[2])
                  << " (" << ctx.gpr[2] << ")\n";
        return;
    }
    if (target_pc == kArchiveFileCopy) {
        std::cerr << "[tow1-archive] copy done dest=" << psprecomp::hex32(ctx.gpr[4])
                  << " first=" << psprecomp::hex32(ctx.gpr[5]) << "\n";
        return;
    }
    if (target_pc == kArchiveFileOpen) {
        std::string path;
        if (g_last_archive_open_path != 0u &&
            rt.memory().contains(g_last_archive_open_path, 1u))
            path = rt.memory().read_c_string(g_last_archive_open_path, 256u);
        std::cerr << "[tow1-archive] open result file=" << psprecomp::hex32(ctx.gpr[2])
                  << " path=\"" << path << "\"\n";
        if (ctx.gpr[2] != 0u && rt.memory().contains(ctx.gpr[2], 0x20u)) {
            const std::uint32_t file = ctx.gpr[2];
            const std::uint32_t vtable = rt.memory().load32(file);
            std::cerr << "[tow1-archive]   file=" << psprecomp::hex32(file)
                      << " vtable=" << psprecomp::hex32(vtable);
            if (rt.memory().contains(vtable, 16u)) {
                std::cerr << " vt:";
                for (std::uint32_t index = 0u; index < 4u; ++index)
                    std::cerr << " " << psprecomp::hex32(
                        rt.memory().load32(vtable + index * 4u));
            }
            std::cerr << "\n";
        }
        return;
    }
    if (target_pc != kGameHeapAlloc) return;
    if (g_heap_diag_calls <= 64u || g_heap_diag_last_size == 196u) {
        const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
        std::cerr << "[tow1-heap] result call=" << g_heap_diag_calls
                  << " uid=" << g_scheduler.current_uid
                  << " name=\"" << (current != g_scheduler.threads.end() ? current->second.name : "?")
                  << "\" heap=" << psprecomp::hex32(g_heap_diag_last_heap)
                  << " request=" << g_heap_diag_last_size
                  << " result=" << psprecomp::hex32(ctx.gpr[2]) << "\n";
        if (ctx.gpr[2] == 0u) {
            dump_heap(rt, "failed", g_heap_diag_last_heap, g_heap_diag_last_size);
        }
    }
}

// PSP priorities are inverted: a smaller number is a higher priority.
std::uint32_t priority_of(std::int32_t uid) {
    const auto found = g_scheduler.threads.find(uid);
    return found != g_scheduler.threads.end() ? found->second.priority : 0xFFFFFFFFu;
}

void enqueue_thread(std::int32_t uid, const psprecomp::AllegrexContext &context) {
    auto thread = g_scheduler.threads.find(uid);
    if (thread == g_scheduler.threads.end()) return;
    thread->second.state = ThreadState::Ready;
    thread->second.context = context;

    const auto existing = std::find_if(
        g_scheduler.ready.begin(), g_scheduler.ready.end(),
        [uid](const ReadyEntry &entry) { return entry.uid == uid; });
    if (existing != g_scheduler.ready.end()) return; // preserve FIFO position

    g_scheduler.ready.push_back(ReadyEntry{uid, g_scheduler.next_sequence++});
}

void promote_expired_delays() {
    struct Expired {
        std::int32_t uid;
        std::uint64_t deadline;
        std::uint64_t sequence;
    };
    std::vector<Expired> expired;
    for (const auto &[uid, thread] : g_scheduler.threads) {
        if (thread.state == ThreadState::Delayed &&
            thread.delay_until_us <= g_scheduler.now_us) {
            expired.push_back(Expired{uid, thread.delay_until_us, thread.delay_sequence});
        }
    }
    std::sort(expired.begin(), expired.end(),
              [](const Expired &left, const Expired &right) {
                  if (left.deadline != right.deadline) return left.deadline < right.deadline;
                  if (left.sequence != right.sequence) return left.sequence < right.sequence;
                  return left.uid < right.uid;
              });
    for (const Expired &item : expired) {
        const auto thread = g_scheduler.threads.find(item.uid);
        if (thread != g_scheduler.threads.end()) {
            enqueue_thread(item.uid, thread->second.context);
        }
    }
}

auto best_ready_thread() {
    return std::min_element(
        g_scheduler.ready.begin(), g_scheduler.ready.end(),
        [](const ReadyEntry &left, const ReadyEntry &right) {
            const std::uint32_t left_priority = priority_of(left.uid);
            const std::uint32_t right_priority = priority_of(right.uid);
            if (left_priority != right_priority) return left_priority < right_priority;
            return left.sequence < right.sequence;
        });
}

bool activate_next_thread(psprecomp::AllegrexContext &ctx, const char *reason) {
    promote_expired_delays();
    if (g_scheduler.ready.empty()) {
        std::uint64_t earliest = std::numeric_limits<std::uint64_t>::max();
        for (const auto &[uid, thread] : g_scheduler.threads) {
            (void)uid;
            if (thread.state == ThreadState::Delayed) {
                earliest = std::min(earliest, thread.delay_until_us);
            }
        }
        if (earliest != std::numeric_limits<std::uint64_t>::max()) {
            g_scheduler.now_us = std::max(g_scheduler.now_us, earliest);
            promote_expired_delays();
        }
    }
    if (g_scheduler.ready.empty()) return false;

    const auto selected = best_ready_thread();
    const std::int32_t uid = selected->uid;
    g_scheduler.ready.erase(selected);

    const auto thread = g_scheduler.threads.find(uid);
    if (thread == g_scheduler.threads.end()) return activate_next_thread(ctx, reason);

    thread->second.state = ThreadState::Running;
    g_scheduler.current_uid = uid;
    ctx = thread->second.context;
    psprecomp::set_runtime_thread_identity(uid, thread->second.name);

    if (thread_diag()) {
        std::cerr << "[tow1] switch reason=" << reason << " uid=" << uid
                  << " name=" << thread->second.name
                  << " pc=" << psprecomp::hex32(ctx.pc)
                  << " sp=" << psprecomp::hex32(ctx.gpr[29])
                  << " ready=" << g_scheduler.ready.size() << "\n";
    }
    return true;
}

void complete_current_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx);

void delete_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0) uid = g_scheduler.current_uid;
    if (uid == g_scheduler.current_uid) {
        complete_current_thread(runtime, ctx);
        return;
    }
    const auto thread = g_scheduler.threads.find(uid);
    if (thread == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    const std::string name = thread->second.name;
    thread->second.state = ThreadState::Completed;
    g_scheduler.ready.erase(
        std::remove_if(g_scheduler.ready.begin(), g_scheduler.ready.end(),
                       [uid](const ReadyEntry &entry) { return entry.uid == uid; }),
        g_scheduler.ready.end());
    if (const auto waiters = g_scheduler.end_waiters.find(uid);
        waiters != g_scheduler.end_waiters.end()) {
        for (ThreadWaiter &waiter : waiters->second) {
            waiter.context.set_gpr(2, 0u);
            enqueue_thread(waiter.uid, waiter.context);
        }
        g_scheduler.end_waiters.erase(waiters);
    }
    g_scheduler.threads.erase(uid);
    if (std::getenv("PSPRECOMP_TOW1_EXIT_DIAG") != nullptr) {
        std::cerr << "[tow1-exit] deleted non-current thread uid=" << uid
                  << " name=\"" << name << "\"\n";
    }
    ctx.set_gpr(2, 0u);
}

void complete_current_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t completed_uid = g_scheduler.current_uid;
    std::uint32_t exit_status = 0u;
    if (auto current = g_scheduler.threads.find(completed_uid);
        current != g_scheduler.threads.end()) {
        current->second.state = ThreadState::Completed;
        exit_status = current->second.exit_status;
    }
    g_scheduler.ready.erase(
        std::remove_if(g_scheduler.ready.begin(), g_scheduler.ready.end(),
                       [completed_uid](const ReadyEntry &entry) {
                           return entry.uid == completed_uid;
                       }),
        g_scheduler.ready.end());

    const auto waiters = g_scheduler.end_waiters.find(completed_uid);
    if (waiters != g_scheduler.end_waiters.end()) {
        for (ThreadWaiter &waiter : waiters->second) {
            waiter.context.set_gpr(2, exit_status);
            if (waiter.status_pointer != 0u &&
                runtime.memory().contains(waiter.status_pointer, 4u)) {
                runtime.memory().store32(waiter.status_pointer, exit_status);
            }
            enqueue_thread(waiter.uid, waiter.context);
        }
        g_scheduler.end_waiters.erase(waiters);
    }

    if (!activate_next_thread(ctx, "thread-complete")) {
        ctx.set_gpr(2, 0u);
        runtime.stop("All PSP threads completed");
    }
}

void thread_return_trampoline(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    complete_current_thread(runtime, ctx);
}



void signal_sleep_semaphore_for_vblank() {
    if (g_sleep_semaphore_uid < 0) return;
    const auto semaphore = g_semaphores.find(g_sleep_semaphore_uid);
    if (semaphore == g_semaphores.end() || semaphore->second.waiters.empty()) return;
    SemaphoreWaiter waiter = semaphore->second.waiters.front();
    semaphore->second.waiters.erase(semaphore->second.waiters.begin());
    psprecomp::AllegrexContext context = waiter.context;
    context.set_gpr(2, 0u);
    enqueue_thread(waiter.uid, context);
}

void starvation_preempt(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    (void)runtime;
    // ToW1's main loop waits on a semaphore named "SleepSema" that the real
    // vblank/timer path signals. The execution-driven tick below models that.
    // Opt out with PSPRECOMP_TOW1_NO_VBLANK_SEMA=1.
    static const bool vblank_sema =
        std::getenv("PSPRECOMP_TOW1_NO_VBLANK_SEMA") == nullptr;
    if (vblank_sema) {
        // Approximate execution-driven time: one 59.94 Hz vblank per ~3 ticks.
        g_scheduler.now_us += 5000u;
        if (g_scheduler.now_us >= g_next_vblank_us) {
            g_next_vblank_us = g_scheduler.now_us + 16683u;
            signal_sleep_semaphore_for_vblank();
        }
    }
    if (g_scheduler.ready.empty()) return;
    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end() || current->second.state != ThreadState::Running) {
        return;
    }
    // The hook runs at an outer-dispatch boundary, so ctx.pc is exactly the
    // next instruction. Save it as-is; do not treat it as an HLE return.
    enqueue_thread(g_scheduler.current_uid, ctx);
    (void)activate_next_thread(ctx, "timeslice");
}

bool allocate_thread_stack(psprecomp::Runtime &runtime, std::uint32_t size,
                           std::uint32_t &top, std::uint32_t &bottom) {
    top = g_scheduler.next_stack_top & ~0xFFu;
    if (top < size) return false;
    bottom = top - size;
    // The partition arena grows upward; stacks must not descend into it.
    if (bottom < g_partitions.next_address) return false;
    if (!runtime.memory().contains(bottom, size)) return false;
    g_scheduler.next_stack_top = bottom;
    return true;
}

void create_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    const std::uint32_t entry = ctx.gpr[5];
    const std::uint32_t priority = ctx.gpr[6];
    const std::uint32_t requested_stack = ctx.gpr[7];
    const std::uint32_t attributes = ctx.gpr[8];

    if (requested_stack < 0x200u) {
        ctx.set_gpr(2, 0x80020194u); // SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE
        return;
    }
    const std::uint32_t stack_size = (requested_stack + 0xFFu) & ~0xFFu;
    std::uint32_t stack_top = 0u;
    std::uint32_t stack_bottom = 0u;
    if (!allocate_thread_stack(runtime, stack_size, stack_top, stack_bottom)) {
        ctx.set_gpr(2, 0x80020190u); // SCE_KERNEL_ERROR_NO_MEMORY
        return;
    }

    std::string name = "unnamed";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }

    const std::int32_t uid = g_scheduler.next_uid++;
    ThreadRecord record{};
    record.name = std::move(name);
    record.entry = entry;
    record.priority = priority;
    record.stack_size = stack_size;
    record.attributes = attributes;
    record.stack_top = stack_top;
    record.stack_bottom = stack_bottom;
    record.kernel_context = stack_top - 0x100u;
    record.state = ThreadState::Created;

    runtime.memory().zero(stack_bottom, stack_size);
    runtime.memory().store32(stack_bottom, static_cast<std::uint32_t>(uid));
    runtime.memory().store32(record.kernel_context + 0xC0u, static_cast<std::uint32_t>(uid));
    runtime.memory().store32(record.kernel_context + 0xC8u, stack_bottom);
    runtime.memory().store32(record.kernel_context + 0xF8u, 0xFFFFFFFFu);
    runtime.memory().store32(record.kernel_context + 0xFCu, 0xFFFFFFFFu);

    std::cerr << "[tow1] create thread uid=" << uid << " name=\"" << record.name
              << "\" entry=" << psprecomp::hex32(entry)
              << " priority=" << priority
              << " stack=" << psprecomp::hex32(stack_bottom) << "-"
              << psprecomp::hex32(stack_top) << "\n";

    g_scheduler.threads.emplace(uid, std::move(record));
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void start_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto found = g_scheduler.threads.find(uid);
    if (found == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u); // UNKNOWN_THID
        return;
    }
    ThreadRecord &thread = found->second;
    if (thread.state != ThreadState::Created) {
        ctx.set_gpr(2, 0x800201A4u); // NOT_DORMANT
        return;
    }

    const std::uint32_t arg_size = ctx.gpr[5];
    const std::uint32_t arg_pointer = ctx.gpr[6];
    std::uint32_t sp = thread.kernel_context;

    psprecomp::AllegrexContext next{};
    if (arg_pointer != 0u && arg_size != 0u) {
        if (!runtime.memory().contains(arg_pointer, arg_size)) {
            ctx.set_gpr(2, 0x800200D3u); // ILLEGAL_ADDR
            return;
        }
        const std::uint32_t aligned_args = (arg_size + 0xFu) & ~0xFu;
        if (sp < thread.stack_bottom + aligned_args + 64u) {
            ctx.set_gpr(2, 0x800200D3u);
            return;
        }
        sp -= aligned_args;
        std::vector<std::uint8_t> arguments(arg_size);
        runtime.memory().copy_out(arg_pointer, arguments);
        runtime.memory().copy_in(sp, arguments);
        next.set_gpr(4, arg_size);
        next.set_gpr(5, sp);
    } else {
        next.set_gpr(4, 0u);
        next.set_gpr(5, 0u);
    }

    sp -= 64u; // PSP kernel frame below the argument copy
    next.set_gpr(26, thread.kernel_context);
    // The title's newlib uses $k0 as a module context pointer: [k0+4] points
    // at a module-info block whose +0x37C field is the module UID. The guest
    // initializes this for the module-start thread; propagate the same block
    // into each new thread's kernel context so its _getmodreent() lookup does
    // not fall back with "no reent structure".
    if (ctx.gpr[26] != 0u && runtime.memory().contains(ctx.gpr[26] + 4u, 4u)) {
        const std::uint32_t module_info = runtime.memory().load32(ctx.gpr[26] + 4u);
        if (module_info != 0u)
            runtime.memory().store32(thread.kernel_context + 4u, module_info);
    }
    next.set_gpr(28, ctx.gpr[28]);
    next.set_gpr(29, sp);
    next.set_gpr(30, sp);
    next.set_gpr(31, kThreadReturnPc);
    next.pc = thread.entry;
    enqueue_thread(uid, next);

    const std::int32_t caller_uid = g_scheduler.current_uid;
    const std::uint32_t caller_priority = priority_of(caller_uid);

    std::cerr << "[tow1] start thread uid=" << uid << " name=\"" << thread.name
              << "\" entry=" << psprecomp::hex32(thread.entry)
              << " priority=" << thread.priority << " caller=" << caller_uid
              << " caller_priority=" << caller_priority << "\n";

    // Starting a thread only preempts when its numeric PSP priority is
    // strictly higher. Otherwise it stays ready and runs at the next boundary.
    if (thread.priority < caller_priority) {
        psprecomp::AllegrexContext caller = ctx;
        caller.set_gpr(2, 0u);
        caller.pc = ctx.gpr[31];
        enqueue_thread(caller_uid, caller);
        if (!activate_next_thread(ctx, "start-thread")) ctx.set_gpr(2, 0u);
    } else {
        ctx.set_gpr(2, 0u);
    }
}

void delay_current_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                          std::uint32_t microseconds) {
    (void)runtime;
    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Delayed;
    current->second.delay_until_us = g_scheduler.now_us + microseconds;
    current->second.delay_sequence = g_scheduler.next_sequence++;

    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;

    if (!activate_next_thread(ctx, "delay")) {
        // Nobody else is runnable: advancing to this deadline is the only
        // legal wakeup. Keep the same thread running with the call returned.
        g_scheduler.now_us = current->second.delay_until_us;
        current->second.state = ThreadState::Running;
        ctx.set_gpr(2, 0u);
    }
}

void wait_thread_end(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (mpeg_diag()) {
        const auto target = g_scheduler.threads.find(uid);
        std::cerr << "[tow1-mpeg] wait-thread-end uid=" << uid
                  << " name=\""
                  << (target != g_scheduler.threads.end() ? target->second.name : "?")
                  << "\" state="
                  << (target != g_scheduler.threads.end()
                          ? static_cast<int>(target->second.state) : -1)
                  << "\n";
    }
    if (uid == g_scheduler.current_uid) {
        ctx.set_gpr(2, 0x800201A4u);
        return;
    }
    const auto target = g_scheduler.threads.find(uid);
    if (target == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }

    const std::uint32_t status_pointer = ctx.gpr[5];

    if (target->second.state == ThreadState::Completed) {
        const std::uint32_t exit_status = target->second.exit_status;
        if (status_pointer != 0u && runtime.memory().contains(status_pointer, 4u)) {
            runtime.memory().store32(status_pointer, exit_status);
        }
        ctx.set_gpr(2, exit_status);
        return;
    }

    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Waiting;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    g_scheduler.end_waiters[uid].push_back(
        ThreadWaiter{g_scheduler.current_uid, status_pointer, suspended});

    if (!activate_next_thread(ctx, "wait-thread-end")) {
        runtime.stop("PSP scheduler deadlock waiting for thread " + std::to_string(uid));
    }
}

void get_system_param_int(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t id = ctx.gpr[4];
    const std::uint32_t destination = ctx.gpr[5];
    std::int32_t value = 0;
    switch (id) {
    case 1: value = 1; break; // language: English (Europe build)
    case 2: value = 0; break; // button preference: leave the game default
    case 3: value = 0; break; // button preference (alternate id)
    case 4: value = 0; break; // parental level
    case 5: value = 1; break; // time format: 24h
    case 6: value = 2; break; // date format: DD/MM/YYYY
    case 7: value = 0; break; // timezone offset (minutes)
    case 8: value = 0; break; // daylight savings: standard
    case 9: value = 0; break; // adhoc channel: automatic
    case 10: value = 0; break; // WLAN power save: off
    default:
        ctx.set_gpr(2, 0x80110103u); // INVALID_SYSTEM_PARAM_ID
        return;
    }
    if (destination == 0u || !runtime.memory().contains(destination, 4u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    runtime.memory().store32(destination, static_cast<std::uint32_t>(value));
    ctx.set_gpr(2, 0u);
}

void get_system_param_string(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t id = ctx.gpr[4];
    const std::uint32_t destination = ctx.gpr[5];
    const std::uint32_t size = ctx.gpr[6];
    if (id != 1u) { // PSP_SYSTEMPARAM_ID_STRING_NICKNAME
        ctx.set_gpr(2, 0x80110103u);
        return;
    }
    if (destination == 0u || size < 2u || !runtime.memory().contains(destination, size)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    const std::string nickname = "Player";
    std::vector<std::uint8_t> bytes(nickname.begin(), nickname.end());
    bytes.push_back(0u);
    if (bytes.size() > size) bytes.resize(size);
    runtime.memory().copy_in(destination, bytes);
    ctx.set_gpr(2, 0u);
}

bool event_flag_satisfied(std::uint32_t pattern, const EventFlagWaiter &waiter) {
    const bool and_mode = (waiter.mode & 1u) != 0u;
    return and_mode ? ((pattern & waiter.bits) == waiter.bits)
                    : ((pattern & waiter.bits) != 0u);
}

void wake_event_flag_waiters(psprecomp::Runtime &runtime, EventFlagRecord &flag) {
    auto waiter = flag.waiters.begin();
    while (waiter != flag.waiters.end()) {
        if (!event_flag_satisfied(flag.pattern, *waiter)) {
            ++waiter;
            continue;
        }
        if (waiter->output_pointer != 0u &&
            runtime.memory().contains(waiter->output_pointer, 4u)) {
            runtime.memory().store32(waiter->output_pointer, flag.pattern);
        }
        if ((waiter->mode & 4u) != 0u) flag.pattern &= ~waiter->bits; // CLEAR
        psprecomp::AllegrexContext context = waiter->context;
        context.set_gpr(2, 0u);
        enqueue_thread(waiter->uid, context);
        waiter = flag.waiters.erase(waiter);
    }
}

void create_event_flag(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    const std::uint32_t initial = ctx.gpr[6];
    std::string name = "event";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_event_flag_uid++;
    EventFlagRecord record{};
    record.name = std::move(name);
    record.pattern = initial;
    g_event_flags.emplace(uid, std::move(record));
    if (thread_diag()) {
        std::cerr << "[tow1] create event flag uid=" << uid << " name=\""
                  << g_event_flags[uid].name << "\" pattern="
                  << psprecomp::hex32(initial) << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void set_event_flag(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    flag->second.pattern |= ctx.gpr[5];
    wake_event_flag_waiters(runtime, flag->second);
    ctx.set_gpr(2, 0u);
}

void clear_event_flag(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    flag->second.pattern &= ~ctx.gpr[5];
    ctx.set_gpr(2, 0u);
}

void wait_event_flag(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    EventFlagWaiter waiter{};
    waiter.uid = g_scheduler.current_uid;
    waiter.bits = ctx.gpr[5];
    waiter.mode = ctx.gpr[6];
    waiter.output_pointer = ctx.gpr[7];
    if (event_flag_satisfied(flag->second.pattern, waiter)) {
        if (waiter.output_pointer != 0u &&
            runtime.memory().contains(waiter.output_pointer, 4u)) {
            runtime.memory().store32(waiter.output_pointer, flag->second.pattern);
        }
        if ((waiter.mode & 4u) != 0u) flag->second.pattern &= ~waiter.bits;
        ctx.set_gpr(2, 0u);
        return;
    }

    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Waiting;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    waiter.context = suspended;
    flag->second.waiters.push_back(waiter);
    if (!activate_next_thread(ctx, "wait-event-flag")) {
        runtime.stop("PSP scheduler deadlock waiting for event flag " +
                     std::to_string(uid));
    }
}

void refer_event_flag_status(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t status_pointer = ctx.gpr[5];
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end() ||
        status_pointer == 0u ||
        !runtime.memory().contains(status_pointer, 52u)) {
        ctx.set_gpr(2, 0x80020132u); // SCE_KERNEL_ERROR_UNKNOWN_EVFID
        return;
    }
    runtime.memory().zero(status_pointer, 52u);
    runtime.memory().store32(status_pointer + 0u, 52u);
    std::string name = flag->second.name;
    if (name.size() > 31u) name.resize(31u);
    if (!name.empty())
        runtime.memory().copy_in(status_pointer + 4u,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(name.data()), name.size()));
    runtime.memory().store32(status_pointer + 36u, 0u);
    runtime.memory().store32(status_pointer + 40u, flag->second.pattern);
    runtime.memory().store32(status_pointer + 44u, flag->second.pattern);
    runtime.memory().store32(status_pointer + 48u,
                             static_cast<std::uint32_t>(flag->second.waiters.size()));
    if (mpeg_diag()) {
        static std::uint32_t logged = 0u;
        if (logged < 20u) {
            ++logged;
            std::cerr << "[tow1-mpeg] event-flag uid=" << uid
                      << " name=\"" << flag->second.name << "\""
                      << " pattern=" << psprecomp::hex32(flag->second.pattern) << "\n";
        }
    }
    ctx.set_gpr(2, 0u);
}

void delete_event_flag(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    for (EventFlagWaiter &waiter : flag->second.waiters) {
        psprecomp::AllegrexContext context = waiter.context;
        context.set_gpr(2, 0x800201A7u);
        enqueue_thread(waiter.uid, context);
    }
    g_event_flags.erase(flag);
    ctx.set_gpr(2, 0u);
}

void sleep_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Sleeping;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    if (!activate_next_thread(ctx, "sleep-thread")) {
        runtime.stop("All PSP threads are sleeping");
    }
}

void wakeup_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0) uid = g_scheduler.current_uid;
    const auto thread = g_scheduler.threads.find(uid);
    if (thread == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    if (thread->second.state != ThreadState::Sleeping) {
        ctx.set_gpr(2, 0x800201A2u);
        return;
    }
    psprecomp::AllegrexContext context = thread->second.context;
    context.set_gpr(2, 0u);
    enqueue_thread(uid, context);
    ctx.set_gpr(2, 0u);
}

bool is_device_root(const std::string &psp_path) {
    const auto colon = psp_path.find(':');
    if (colon == std::string::npos) return false;
    std::string rest = psp_path.substr(colon + 1u);
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
        rest.erase(rest.begin());
    }
    return rest.empty();
}

std::filesystem::path umd_image_path(psprecomp::Runtime &runtime) {
    if (const char *configured = std::getenv("PSPRECOMP_TOW1_ISO");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    return runtime.game_root().parent_path() / "tow1.iso";
}

std::int32_t open_host_file(psprecomp::Runtime &runtime, const std::string &psp_path,
                            std::uint32_t flags) {
    if (is_device_root(psp_path)) {
        const std::filesystem::path image = umd_image_path(runtime);
        std::error_code device_error;
        if (!std::filesystem::is_regular_file(image, device_error)) {
            if (file_diag()) {
                std::cerr << "[tow1][io] device ENOENT \"" << psp_path << "\" -> "
                          << image.string() << "\n";
            }
            return 0x80010002;
        }
        FileHandle handle{};
        handle.stream.open(image, std::ios::binary);
        if (!handle.stream) return 0x80010001;
        handle.size = std::filesystem::file_size(image, device_error);
        handle.position = 0;
        handle.psp_path = psp_path;
        handle.device = true;
        const std::int32_t fd = g_next_fd++;
        g_files.emplace(fd, std::move(handle));
        if (file_diag()) {
            std::cerr << "[tow1][io] open device fd=" << fd
                      << " image=\"" << image.string() << "\" size="
                      << g_files[fd].size << "\n";
        }
        return fd;
    }

    const std::filesystem::path host_path = runtime.translate_path(psp_path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(host_path, error)) {
        if (file_diag()) {
            std::cerr << "[tow1][io] open ENOENT \"" << psp_path << "\" -> "
                      << host_path.string() << "\n";
        }
        return 0x80010002; // ENOENT
    }
    FileHandle handle{};
    handle.stream.open(host_path, std::ios::binary);
    if (!handle.stream) return 0x80010001; // EACCES
    handle.size = std::filesystem::file_size(host_path, error);
    handle.position = 0;
    handle.psp_path = psp_path;
    handle.writable = (flags & 3u) != 1u; // O_WRONLY or O_RDWR
    if (host_path.extension() == ".pmf") g_pmf_host_path = host_path.string();

    const std::int32_t fd = g_next_fd++;
    g_files.emplace(fd, std::move(handle));
    if (file_diag()) {
        std::cerr << "[tow1][io] open fd=" << fd << " size=" << g_files[fd].size
                  << " \"" << psp_path << "\"\n";
    }
    return fd;
}

void io_open(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(
        open_host_file(runtime, path, ctx.gpr[5])));
}

void io_open_async(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(
        open_host_file(runtime, path, ctx.gpr[5])));
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[2]);
    if (fd >= 0) {
        const auto handle = g_files.find(fd);
        if (handle != g_files.end()) handle->second.async_result = 0;
    }
}

void io_close(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    ctx.set_gpr(2, g_files.erase(fd) == 1u ? 0u : 0x80010009u);
}

std::int32_t read_host_file(psprecomp::Runtime &runtime, std::int32_t fd,
                            std::uint32_t buffer, std::uint32_t size,
                            std::uint32_t caller_ra = 0u) {
    const auto handle = g_files.find(fd);
    if (handle == g_files.end()) return 0x80010009; // EBADF
    if (size == 0u) return 0;

    // The raw UMD device is addressed in 2048-byte sectors by this title:
    // sceIoRead's count is a sector count, and the return value is the number
    // of sectors read. Normal files use byte counts.
    constexpr std::uint64_t kSectorSize = 2048u;
    const bool device = handle->second.device;
    const std::uint64_t requested =
        device ? static_cast<std::uint64_t>(size) * kSectorSize
               : static_cast<std::uint64_t>(size);
    if (requested > std::numeric_limits<std::uint32_t>::max() ||
        buffer == 0u ||
        !runtime.memory().contains(buffer, static_cast<std::size_t>(requested)))
        return 0x80010014;

    const std::uint64_t remaining =
        handle->second.size > handle->second.position
            ? handle->second.size - handle->second.position : 0u;
    const std::size_t to_read =
        static_cast<std::size_t>(std::min<std::uint64_t>(requested, remaining));
    if (to_read == 0u) return 0;

    std::vector<std::uint8_t> data(to_read);
    handle->second.stream.clear();
    handle->second.stream.seekg(static_cast<std::streamoff>(handle->second.position));
    handle->second.stream.read(reinterpret_cast<char *>(data.data()),
                               static_cast<std::streamsize>(to_read));
    const std::size_t got = static_cast<std::size_t>(handle->second.stream.gcount());
    if (got != 0u) {
        runtime.memory().copy_in(buffer, std::span<const std::uint8_t>(data.data(), got));
        handle->second.position += got;
    }
    const bool title_archive_read =
        handle->second.psp_path.find("title.arc") != std::string::npos ||
        handle->second.psp_path.find("umd") != std::string::npos;
    if (file_diag() && (g_read_diag_count < 24u || title_archive_read)) {
        if (g_read_diag_count < 24u) ++g_read_diag_count;
        const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
        std::cerr << "[tow1][io] read fd=" << fd << " request=" << size
                  << (device ? " sectors" : " bytes")
                  << " bytes=" << got << " pos=" << handle->second.position
                  << " buf=" << psprecomp::hex32(buffer)
                  << " ra=" << psprecomp::hex32(caller_ra)
                  << " uid=" << g_scheduler.current_uid
                  << " name=\"" << (current != g_scheduler.threads.end() ? current->second.name : "?")
                  << "\" path=\"" << handle->second.psp_path << "\"\n";
    }
    if (device)
        return static_cast<std::int32_t>(got / kSectorSize);
    return static_cast<std::int32_t>(got);
}

void io_read(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(2, static_cast<std::uint32_t>(read_host_file(
        runtime, static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5], ctx.gpr[6],
        ctx.gpr[31])));
}

void io_read_async(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t result = read_host_file(runtime, fd, ctx.gpr[5], ctx.gpr[6],
                                               ctx.gpr[31]);
    const auto handle = g_files.find(fd);
    if (handle != g_files.end()) handle->second.async_result = result;
    ctx.set_gpr(2, 0u);
}

void io_wait_async_cb(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t result_pointer = ctx.gpr[5];
    const auto handle = g_files.find(fd);
    const std::int64_t result = handle != g_files.end() ? handle->second.async_result : 0;
    if (result_pointer != 0u && runtime.memory().contains(result_pointer, 8u)) {
        runtime.memory().store32(result_pointer,
                                 static_cast<std::uint32_t>(result));
        runtime.memory().store32(result_pointer + 4u,
                                 static_cast<std::uint32_t>(
                                     static_cast<std::uint64_t>(result) >> 32u));
    }
    ctx.set_gpr(2, 0u);
}

void io_lseek(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    // MIPS o32 aligns a 64-bit argument to an even register pair: the fd is
    // a0, the aligned SceOff occupies a2/a3, and whence is a4 (t0).
    const std::int64_t offset = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(ctx.gpr[6]) |
        (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u));
    const std::int32_t whence = static_cast<std::int32_t>(ctx.gpr[8]);
    const auto handle = g_files.find(fd);
    if (handle == g_files.end()) {
        ctx.set_gpr(2, 0x80010009u);
        ctx.set_gpr(3, 0u);
        return;
    }
    // The raw UMD device is addressed in 2048-byte sectors by this title.
    const std::int64_t byte_offset = handle->second.device ? offset * 2048 : offset;
    std::int64_t position = 0;
    if (whence == 0) position = byte_offset;
    else if (whence == 1) position = static_cast<std::int64_t>(handle->second.position) + byte_offset;
    else position = static_cast<std::int64_t>(handle->second.size) + byte_offset;
    if (position < 0) position = 0;
    if (static_cast<std::uint64_t>(position) > handle->second.size) {
        position = static_cast<std::int64_t>(handle->second.size);
    }
    handle->second.position = static_cast<std::uint64_t>(position);
    handle->second.stream.clear();
    handle->second.stream.seekg(static_cast<std::streamoff>(position));
    if (file_diag() && g_lseek_diag_count < 24u) {
        ++g_lseek_diag_count;
        std::cerr << "[tow1][io] lseek fd=" << fd << " offset=" << offset
                  << " whence=" << whence << " -> " << position
                  << " path=\"" << handle->second.psp_path << "\"\n";
    }
    const std::int64_t logical_position =
        handle->second.device ? position / 2048 : position;
    ctx.set_gpr(2, static_cast<std::uint32_t>(static_cast<std::uint64_t>(logical_position)));
    ctx.set_gpr(3, static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(logical_position) >> 32u));
}

void io_getstat(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    const std::uint32_t stat_pointer = ctx.gpr[5];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    if (is_device_root(path)) {
        const std::filesystem::path image = umd_image_path(runtime);
        std::error_code device_error;
        const std::uint64_t size = std::filesystem::is_regular_file(image, device_error)
            ? std::filesystem::file_size(image, device_error) : 0u;
        if (stat_pointer == 0u || !runtime.memory().contains(stat_pointer, 88u)) {
            ctx.set_gpr(2, 0x80010014u);
            return;
        }
        runtime.memory().zero(stat_pointer, 88u);
        runtime.memory().store32(stat_pointer, 0x21FFu);
        runtime.memory().store32(stat_pointer + 8u,
                                 static_cast<std::uint32_t>(size));
        runtime.memory().store32(stat_pointer + 12u,
                                 static_cast<std::uint32_t>(size >> 32u));
        ctx.set_gpr(2, 0u);
        return;
    }

    const std::filesystem::path host_path = runtime.translate_path(path);
    std::error_code error;
    const bool directory = std::filesystem::is_directory(host_path, error);
    const bool regular = std::filesystem::is_regular_file(host_path, error);
    if (!directory && !regular) {
        ctx.set_gpr(2, 0x80010002u);
        return;
    }
    if (stat_pointer == 0u || !runtime.memory().contains(stat_pointer, 88u)) {
        ctx.set_gpr(2, 0x80010014u);
        return;
    }
    const std::uint64_t size = regular
        ? std::filesystem::file_size(host_path, error) : 0u;
    runtime.memory().zero(stat_pointer, 88u);
    runtime.memory().store32(stat_pointer, directory ? 0x11FFu : 0x21FFu);
    runtime.memory().store32(stat_pointer + 8u, static_cast<std::uint32_t>(size));
    runtime.memory().store32(stat_pointer + 12u,
                             static_cast<std::uint32_t>(size >> 32u));
    ctx.set_gpr(2, 0u);
}

void io_dopen(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    const std::filesystem::path host_path = runtime.translate_path(path);
    std::error_code error;
    if (!std::filesystem::is_directory(host_path, error)) {
        ctx.set_gpr(2, 0x80010002u);
        return;
    }
    DirHandle handle{};
    for (const auto &entry : std::filesystem::directory_iterator(host_path, error)) {
        handle.entries.push_back(entry);
    }
    const std::int32_t uid = g_next_dir_fd++;
    g_dirs.emplace(uid, std::move(handle));
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void io_dread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t entry_pointer = ctx.gpr[5];
    const auto handle = g_dirs.find(uid);
    if (handle == g_dirs.end() || handle->second.next >= handle->second.entries.size()) {
        ctx.set_gpr(2, 0u);
        return;
    }
    if (entry_pointer == 0u || !runtime.memory().contains(entry_pointer, 352u)) {
        ctx.set_gpr(2, 0x80010014u);
        return;
    }
    const std::filesystem::directory_entry &entry =
        handle->second.entries[handle->second.next++];
    std::error_code error;
    const bool directory = entry.is_directory(error);
    const std::uint64_t size = entry.is_regular_file(error)
        ? entry.file_size(error) : 0u;
    runtime.memory().zero(entry_pointer, 352u);
    runtime.memory().store32(entry_pointer, directory ? 0x11FFu : 0x21FFu);
    runtime.memory().store32(entry_pointer + 8u, static_cast<std::uint32_t>(size));
    runtime.memory().store32(entry_pointer + 12u,
                             static_cast<std::uint32_t>(size >> 32u));
    const std::string name = entry.path().filename().string();
    std::vector<std::uint8_t> bytes(name.begin(), name.end());
    bytes.push_back(0u);
    if (bytes.size() > 256u) bytes.resize(256u);
    runtime.memory().copy_in(entry_pointer + 88u, bytes);
    ctx.set_gpr(2, 1u);
}

void io_dclose(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    ctx.set_gpr(2, g_dirs.erase(uid) == 1u ? 0u : 0x80010009u);
}

void io_devctl(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t out_pointer = ctx.gpr[8];
    const std::uint32_t out_size = ctx.gpr[9];
    if (out_pointer != 0u && out_size != 0u &&
        runtime.memory().contains(out_pointer, out_size)) {
        runtime.memory().zero(out_pointer, out_size);
    }
    if (file_diag()) {
        std::cerr << "[tow1][io] devctl cmd=" << psprecomp::hex32(ctx.gpr[5])
                  << " out_size=" << out_size << "\n";
    }
    ctx.set_gpr(2, 0u);
}

void io_ioctl(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t out_pointer = ctx.gpr[8];
    const std::uint32_t out_size = ctx.gpr[9];
    if (out_pointer != 0u && out_size != 0u &&
        runtime.memory().contains(out_pointer, out_size)) {
        runtime.memory().zero(out_pointer, out_size);
    }
    ctx.set_gpr(2, 0u);
}

void io_write(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto handle = g_files.find(fd);
    if (handle == g_files.end()) {
        ctx.set_gpr(2, 0x80010009u);
        return;
    }
    ctx.set_gpr(2, handle->second.writable ? ctx.gpr[6] : 0x80010001u);
}

void io_rename(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t old_pointer = ctx.gpr[4];
    const std::uint32_t new_pointer = ctx.gpr[5];
    std::string old_path;
    std::string new_path;
    if (old_pointer != 0u && runtime.memory().contains(old_pointer, 1u))
        old_path = runtime.memory().read_c_string(old_pointer, 1024u);
    if (new_pointer != 0u && runtime.memory().contains(new_pointer, 1u))
        new_path = runtime.memory().read_c_string(new_pointer, 1024u);
    std::error_code error;
    std::filesystem::rename(runtime.translate_path(old_path),
                            runtime.translate_path(new_path), error);
    ctx.set_gpr(2, error ? 0x80010005u : 0u);
}

void load_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    if (const char *size_text = std::getenv("PSPRECOMP_TOW1_LIBFONT_SIZE");
        size_text != nullptr && *size_text != '\0' &&
        path.find("libfont") != std::string::npos) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(std::strtoul(size_text, nullptr, 0));
        if (value != 0u) runtime.memory().store32(0x08E0E3D4u, value);
    }
    const std::int32_t uid = g_next_module_uid++;
    g_loaded_modules.emplace(uid, LoadedModule{path});
    if (module_diag()) {
        std::cerr << "[tow1][module] load uid=" << uid << " path=\"" << path
                  << "\" flags=" << psprecomp::hex32(ctx.gpr[5]) << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void start_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t status_pointer = ctx.gpr[7];
    if (status_pointer != 0u && runtime.memory().contains(status_pointer, 4u)) {
        runtime.memory().store32(status_pointer, 0u);
    }
    if (module_diag()) {
        std::cerr << "[tow1][module] start uid=" << uid << "\n";
    }
    ctx.set_gpr(2, 0u);
}

void stop_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t status_pointer = ctx.gpr[7];
    if (status_pointer != 0u && runtime.memory().contains(status_pointer, 4u)) {
        runtime.memory().store32(status_pointer, 0u);
    }
    ctx.set_gpr(2, 0u);
}

void unload_module(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    ctx.set_gpr(2, g_loaded_modules.erase(uid) == 1u ? 0u : 0x80020130u);
}

void stop_unload_self_module_with_status(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(2, 0u);
}

constexpr std::uint32_t kFontLibHandle = 0x08801000u;
constexpr std::uint32_t kFontHandle = 0x08801100u;

int g_font_pixel_height = 16;

struct HostGlyph {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    int advance = 0;
    std::vector<std::uint8_t> pixels;
};

#if defined(_WIN32)
struct HostFontState {
    HDC dc = nullptr;
    HFONT font = nullptr;
    int height = 0;
};

HostFontState g_host_font;

void ensure_host_font(int pixel_height) {
    if (g_host_font.dc == nullptr) {
        g_host_font.dc = CreateCompatibleDC(nullptr);
    }
    if (g_host_font.font != nullptr && g_host_font.height == pixel_height) return;
    if (g_host_font.font != nullptr) {
        DeleteObject(g_host_font.font);
        g_host_font.font = nullptr;
    }
    LOGFONTW lf{};
    lf.lfHeight = -pixel_height;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_host_font.font = CreateFontIndirectW(&lf);
    if (g_host_font.font == nullptr) {
        wcscpy_s(lf.lfFaceName, L"Arial");
        g_host_font.font = CreateFontIndirectW(&lf);
    }
    g_host_font.height = pixel_height;
    SelectObject(g_host_font.dc, g_host_font.font);
}

bool rasterize_host_glyph(unsigned int code, int pixel_height, HostGlyph &glyph) {
    ensure_host_font(pixel_height);
    if (g_host_font.dc == nullptr || g_host_font.font == nullptr) return false;
    MAT2 mat{};
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    GLYPHMETRICS metrics{};
    if (code > 0xFFFFu) code = '?';
    DWORD size = GetGlyphOutlineW(g_host_font.dc, static_cast<UINT>(code),
                                  GGO_BITMAP, &metrics, 0, nullptr, &mat);
    if (size == GDI_ERROR) {
        code = '?';
        size = GetGlyphOutlineW(g_host_font.dc, static_cast<UINT>(code),
                                GGO_BITMAP, &metrics, 0, nullptr, &mat);
        if (size == GDI_ERROR) return false;
    }
    std::vector<std::uint8_t> raw(size);
    if (size != 0u &&
        GetGlyphOutlineW(g_host_font.dc, static_cast<UINT>(code), GGO_BITMAP,
                         &metrics, size, raw.data(), &mat) == GDI_ERROR) {
        return false;
    }
    glyph.width = static_cast<int>(metrics.gmBlackBoxX);
    glyph.height = static_cast<int>(metrics.gmBlackBoxY);
    glyph.left = metrics.gmptGlyphOrigin.x;
    glyph.top = metrics.gmptGlyphOrigin.y;
    glyph.advance = metrics.gmCellIncX;
    if (glyph.advance <= 0) glyph.advance = pixel_height / 2;
    if (glyph.width <= 0 || glyph.height <= 0) {
        glyph.width = glyph.height = 0;
        return true;
    }
    const int stride = static_cast<int>(((glyph.width + 31) / 32) * 4);
    glyph.pixels.assign(static_cast<std::size_t>(glyph.width * glyph.height), 0u);
    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            const std::uint8_t byte = raw[static_cast<std::size_t>(y * stride + x / 8)];
            const bool set = ((byte >> (7 - (x & 7))) & 1u) != 0u;
            glyph.pixels[static_cast<std::size_t>(y * glyph.width + x)] =
                set ? 0xFFu : 0x00u;
        }
    }
    return true;
}
#else
bool rasterize_host_glyph(unsigned int, int, HostGlyph &glyph) {
    glyph.width = glyph.height = 0;
    return false;
}
#endif

float load_float(psprecomp::GuestMemory &memory, std::uint32_t address) {
    const std::uint32_t bits = memory.load32(address);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    memory.store32(address, bits);
}

void font_new_lib(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        std::cerr << "[tow1-font] new-lib data=" << psprecomp::hex32(ctx.gpr[4])
                  << " error=" << psprecomp::hex32(ctx.gpr[5]) << "\n";
    }
    const std::uint32_t error_pointer = ctx.gpr[5];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, kFontLibHandle);
}

void font_open(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        std::cerr << "[tow1-font] open lib=" << psprecomp::hex32(ctx.gpr[4])
                  << " index=" << ctx.gpr[5]
                  << " mode=" << psprecomp::hex32(ctx.gpr[6]) << "\n";
    }
    const std::uint32_t error_pointer = ctx.gpr[7];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, kFontHandle);
}

void font_find_optimum(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t style_pointer = ctx.gpr[5];
    if (style_pointer != 0u && runtime.memory().contains(style_pointer, 8u)) {
        const float style_height = load_float(runtime.memory(), style_pointer + 4u);
        if (style_height > 0.0f) {
            g_font_pixel_height = std::clamp(
                static_cast<int>(std::lround(style_height)), 6, 48);
        }
        if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
            std::cerr << "[tow1-font] optimum width="
                      << load_float(runtime.memory(), style_pointer)
                      << " height=" << style_height
                      << " pixel=" << g_font_pixel_height << "\n";
        }
    }
    const std::uint32_t error_pointer = ctx.gpr[6];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, 0u); // font index 0
}

void font_get_info(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        std::cerr << "[tow1-font] get-info handle=" << psprecomp::hex32(ctx.gpr[4])
                  << " info=" << psprecomp::hex32(ctx.gpr[5]) << "\n";
    }
    const std::uint32_t info_pointer = ctx.gpr[5];
    if (info_pointer == 0u || !runtime.memory().contains(info_pointer, 264u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    const int pixel = std::clamp(g_font_pixel_height, 6, 48);
    runtime.memory().zero(info_pointer, 264u);
    // SceFontInfo: maxFixed at +0, maxFloat at +40, maxWidth +80,
    // maxHeight +82, characters +84, subCharacters +88, fontStyle +92,
    // depth +260, unknown +261.
    runtime.memory().store32(info_pointer + 0u, static_cast<std::uint32_t>(pixel));
    runtime.memory().store32(info_pointer + 4u, static_cast<std::uint32_t>(pixel));
    runtime.memory().store32(info_pointer + 8u, static_cast<std::uint32_t>(pixel));
    runtime.memory().store32(info_pointer + 12u, 0u);
    runtime.memory().store32(info_pointer + 16u,
                             static_cast<std::uint32_t>(pixel) << 24u);
    runtime.memory().store32(info_pointer + 20u, 0u);
    runtime.memory().store32(info_pointer + 24u, 0u);
    runtime.memory().store32(info_pointer + 28u,
                             static_cast<std::uint32_t>(pixel) << 24u);
    store_float(runtime.memory(), info_pointer + 40u, static_cast<float>(pixel));
    store_float(runtime.memory(), info_pointer + 44u, static_cast<float>(pixel));
    store_float(runtime.memory(), info_pointer + 48u, static_cast<float>(pixel));
    store_float(runtime.memory(), info_pointer + 52u, 0.0f);
    store_float(runtime.memory(), info_pointer + 56u,
                static_cast<float>(pixel) * 0.5f);
    store_float(runtime.memory(), info_pointer + 60u, 0.0f);
    store_float(runtime.memory(), info_pointer + 64u, 0.0f);
    store_float(runtime.memory(), info_pointer + 68u, static_cast<float>(pixel));
    store_float(runtime.memory(), info_pointer + 72u, 0.0f);
    store_float(runtime.memory(), info_pointer + 76u, 0.0f);
    runtime.memory().store16(info_pointer + 80u, static_cast<std::uint16_t>(pixel));
    runtime.memory().store16(info_pointer + 82u, static_cast<std::uint16_t>(pixel));
    runtime.memory().store32(info_pointer + 84u, 0x10000u);
    runtime.memory().store32(info_pointer + 88u, 0u);
    store_float(runtime.memory(), info_pointer + 92u, static_cast<float>(pixel));
    store_float(runtime.memory(), info_pointer + 96u, static_cast<float>(pixel));
    runtime.memory().store16(info_pointer + 112u, 0u); // family
    runtime.memory().store16(info_pointer + 114u, 1u); // style regular
    runtime.memory().store16(info_pointer + 118u, 2u); // English
    runtime.memory().store8(info_pointer + 260u, 8u);
    ctx.set_gpr(2, 0u);
}

void font_get_char_info(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        static std::uint32_t logged = 0u;
        if (logged < 24u) {
            ++logged;
            std::cerr << "[tow1-font] char-info handle=" << psprecomp::hex32(ctx.gpr[4])
                      << " char=" << psprecomp::hex32(ctx.gpr[5])
                      << " info=" << psprecomp::hex32(ctx.gpr[6]) << "\n";
        }
    }
    const std::uint32_t info_pointer = ctx.gpr[6];
    if (info_pointer == 0u || !runtime.memory().contains(info_pointer, 60u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    HostGlyph glyph;
    rasterize_host_glyph(ctx.gpr[5] & 0xFFFFu, g_font_pixel_height, glyph);
    runtime.memory().zero(info_pointer, 60u);
    const std::uint32_t width = static_cast<std::uint32_t>(
        glyph.width > 0 ? glyph.width : glyph.advance);
    const std::uint32_t height = static_cast<std::uint32_t>(glyph.height);
    runtime.memory().store32(info_pointer + 0u, width);
    runtime.memory().store32(info_pointer + 4u, height);
    runtime.memory().store32(info_pointer + 8u, static_cast<std::uint32_t>(glyph.left));
    runtime.memory().store32(info_pointer + 12u, static_cast<std::uint32_t>(glyph.top));
    // SceFontCharacterData at +16.  Metrics are 26.6 fixed point.
    runtime.memory().store32(info_pointer + 16u,
                             static_cast<std::uint32_t>(glyph.advance) << 6u);
    runtime.memory().store32(info_pointer + 20u,
                             static_cast<std::uint32_t>(g_font_pixel_height) << 6u);
    runtime.memory().store32(info_pointer + 24u,
                             static_cast<std::uint32_t>(glyph.top) << 6u);
    runtime.memory().store32(info_pointer + 28u,
                             static_cast<std::uint32_t>(glyph.top - glyph.height) << 6u);
    runtime.memory().store32(info_pointer + 32u,
                             static_cast<std::uint32_t>(glyph.left) << 6u);
    runtime.memory().store32(info_pointer + 36u,
                             static_cast<std::uint32_t>(glyph.top) << 6u);
    runtime.memory().store32(info_pointer + 40u,
                             static_cast<std::uint32_t>(glyph.left) << 6u);
    runtime.memory().store32(info_pointer + 44u,
                             static_cast<std::uint32_t>(glyph.top) << 6u);
    runtime.memory().store32(info_pointer + 48u,
                             static_cast<std::uint32_t>(glyph.advance) << 6u);
    runtime.memory().store32(info_pointer + 52u, 0u);
    ctx.set_gpr(2, 0u);
}

void font_draw_glyph_to_image(psprecomp::GuestMemory &memory,
                              std::uint32_t image_pointer,
                              const HostGlyph &glyph) {
    if (glyph.width <= 0 || glyph.height <= 0) return;
    const std::uint32_t format = memory.load32(image_pointer + 0u);
    if (format != 0u && format != 2u) return;
    const std::int32_t x_fixed = static_cast<std::int32_t>(
        memory.load32(image_pointer + 4u));
    const std::int32_t y_fixed = static_cast<std::int32_t>(
        memory.load32(image_pointer + 8u));
    const int origin_x = x_fixed >> 6;
    const int origin_y = y_fixed >> 6;
    const int x_fraction = x_fixed & 0x3F;
    const std::uint32_t buffer_width = memory.load16(image_pointer + 12u);
    const std::uint32_t buffer_height = memory.load16(image_pointer + 14u);
    const std::uint32_t bytes_per_line = memory.load16(image_pointer + 16u);
    const std::uint32_t base = memory.load32(image_pointer + 20u);
    if (base == 0u || bytes_per_line == 0u) return;

    auto sample = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= glyph.width || y >= glyph.height) return 0;
        const std::uint8_t value =
            glyph.pixels[static_cast<std::size_t>(y * glyph.width + x)];
        return format == 0u ? (value >> 4) : value;
    };

    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            const int a = sample(x - 1, y);
            const int b = sample(x, y);
            const int blended = (a * x_fraction) / 64 +
                                (b * (64 - x_fraction) + 63) / 64;
            if (blended == 0) continue;
            const int dest_x = origin_x + x;
            const int dest_y = origin_y + y;
            if (dest_x < 0 || dest_y < 0 ||
                dest_x >= static_cast<int>(buffer_width) ||
                dest_y >= static_cast<int>(buffer_height)) {
                continue;
            }
            if (format == 0u) {
                const std::uint32_t address =
                    base + static_cast<std::uint32_t>(dest_y) * bytes_per_line +
                    static_cast<std::uint32_t>(dest_x / 2);
                const int shift = ((dest_x & 1) != 0) ? 4 : 0;
                const std::uint8_t old_value = memory.load8(address);
                const int old_pixel = (old_value >> shift) & 0x0F;
                const int new_pixel = std::min(old_pixel + blended, 15);
                const std::uint8_t new_value = static_cast<std::uint8_t>(
                    (old_value & ~(0x0F << shift)) | (new_pixel << shift));
                memory.store8(address, new_value);
            } else {
                const std::uint32_t address =
                    base + static_cast<std::uint32_t>(dest_y) * bytes_per_line +
                    static_cast<std::uint32_t>(dest_x);
                const int old_pixel = memory.load8(address);
                const int new_pixel = std::min(old_pixel + blended, 255);
                memory.store8(address, static_cast<std::uint8_t>(new_pixel));
            }
        }
    }
}

void font_get_glyph(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        static std::uint32_t logged = 0u;
        if (logged < 24u) {
            ++logged;
            std::cerr << "[tow1-font] glyph handle=" << psprecomp::hex32(ctx.gpr[4])
                      << " char=" << psprecomp::hex32(ctx.gpr[5])
                      << " image=" << psprecomp::hex32(ctx.gpr[6])
                      << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        }
    }
    const std::uint32_t image_pointer = ctx.gpr[6];
    if (image_pointer == 0u || !runtime.memory().contains(image_pointer, 24u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    const std::int32_t image_x = static_cast<std::int32_t>(
        runtime.memory().load32(image_pointer + 4u));
    const std::int32_t image_y = static_cast<std::int32_t>(
        runtime.memory().load32(image_pointer + 8u));
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        static std::uint32_t logged_images = 0u;
        if (logged_images < 4u) {
            ++logged_images;
            std::cerr << "[tow1-font] image-before fmt="
                      << runtime.memory().load32(image_pointer)
                      << " x=" << image_x << " y=" << image_y
                      << " w=" << runtime.memory().load16(image_pointer + 12u)
                      << " h=" << runtime.memory().load16(image_pointer + 14u)
                      << " bpl=" << runtime.memory().load16(image_pointer + 16u)
                      << " data=" << psprecomp::hex32(
                             runtime.memory().load32(image_pointer + 20u)) << "\n";
        }
    }
    HostGlyph glyph;
    rasterize_host_glyph(ctx.gpr[5] & 0xFFFFu, g_font_pixel_height, glyph);
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr) {
        static std::uint32_t dumped = 0u;
        if (dumped < 8u) {
            ++dumped;
            std::size_t ink = 0u;
            for (const std::uint8_t pixel : glyph.pixels) ink += pixel != 0u ? 1u : 0u;
            std::cerr << "[tow1-font] glyph-pixels char=" << psprecomp::hex32(ctx.gpr[5])
                      << " size=" << glyph.width << 'x' << glyph.height
                      << " ink=" << ink << " advance=" << glyph.advance << "\n";
            if (const char *path = std::getenv("PSPRECOMP_TOW1_FONT_GLYPH_DUMP");
                path != nullptr && *path != '\0' &&
                glyph.width > 0 && glyph.height > 0) {
                std::ofstream out(path, std::ios::binary);
                if (out) {
                    out << "P5\n" << glyph.width << ' ' << glyph.height << "\n255\n";
                    out.write(reinterpret_cast<const char *>(glyph.pixels.data()),
                              static_cast<std::streamsize>(glyph.pixels.size()));
                }
            }
        }
    }
    font_draw_glyph_to_image(runtime.memory(), image_pointer, glyph);
    ctx.set_gpr(2, 0u);
}

void font_get_char_image_rect(psprecomp::Runtime &runtime,
                              psprecomp::AllegrexContext &ctx) {
    const std::uint32_t rect_pointer = ctx.gpr[6];
    if (rect_pointer == 0u || !runtime.memory().contains(rect_pointer, 4u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    HostGlyph glyph;
    rasterize_host_glyph(ctx.gpr[5] & 0xFFFFu, g_font_pixel_height, glyph);
    runtime.memory().store16(rect_pointer + 0u,
                             static_cast<std::uint16_t>(std::clamp(glyph.width, 0, 64)));
    runtime.memory().store16(rect_pointer + 2u,
                             static_cast<std::uint16_t>(std::clamp(glyph.height, 0, 64)));
    ctx.set_gpr(2, 0u);
}

void font_open_user(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("PSPRECOMP_TOW1_LIBFONT_DIAG") != nullptr && ctx.gpr[5] != 0u &&
        runtime.memory().contains(ctx.gpr[5], 1u)) {
        std::cerr << "[tow1-font] open-user path=\""
                  << runtime.memory().read_c_string(ctx.gpr[5], 256u) << "\"\n";
    }
    if (ctx.gpr[7] != 0u && runtime.memory().contains(ctx.gpr[7], 4u)) {
        runtime.memory().store32(ctx.gpr[7], 0u);
    }
    ctx.set_gpr(2, kFontHandle);
}

void font_get_num_list(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (ctx.gpr[5] != 0u && runtime.memory().contains(ctx.gpr[5], 4u)) {
        runtime.memory().store32(ctx.gpr[5], 0u);
    }
    ctx.set_gpr(2, 1u);
}

void font_fill_style(psprecomp::Runtime &runtime, std::uint32_t style_pointer) {
    if (style_pointer == 0u || !runtime.memory().contains(style_pointer, 168u)) return;
    runtime.memory().zero(style_pointer, 168u);
    store_float(runtime.memory(), style_pointer + 0u,
                static_cast<float>(g_font_pixel_height));
    store_float(runtime.memory(), style_pointer + 4u,
                static_cast<float>(g_font_pixel_height));
    store_float(runtime.memory(), style_pointer + 16u, 1.0f);
    runtime.memory().store16(style_pointer + 20u, 0u);
    runtime.memory().store16(style_pointer + 22u, 1u);
    runtime.memory().store16(style_pointer + 26u, 2u);
    const char name[] = "Host Sans";
    for (std::size_t index = 0; index + 1u < sizeof(name); ++index) {
        runtime.memory().store8(style_pointer + 32u + static_cast<std::uint32_t>(index),
                                static_cast<std::uint8_t>(name[index]));
    }
}

void font_get_font_list(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t output = ctx.gpr[5];
    const std::uint32_t count = ctx.gpr[6];
    if (output != 0u && count != 0u) font_fill_style(runtime, output);
    ctx.set_gpr(2, count != 0u ? 1u : 0u);
}

void font_get_info_by_index(psprecomp::Runtime &runtime,
                            psprecomp::AllegrexContext &ctx) {
    font_fill_style(runtime, ctx.gpr[5]);
    ctx.set_gpr(2, 0u);
}

void font_find_font(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    font_find_optimum(runtime, ctx);
}

void font_calc_memory_size(psprecomp::Runtime &runtime,
                           psprecomp::AllegrexContext &ctx) {
    if (ctx.gpr[6] != 0u && runtime.memory().contains(ctx.gpr[6], 4u)) {
        runtime.memory().store32(ctx.gpr[6], 0u);
    }
    ctx.set_gpr(2, 0x4000u);
}

void font_shadow_info(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    font_get_char_info(runtime, ctx);
}

void font_shadow_glyph(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    font_get_glyph(runtime, ctx);
}

void font_shadow_rect(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    font_get_char_image_rect(runtime, ctx);
}

void font_glyph_clip(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    font_get_glyph(runtime, ctx);
}

void font_close(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(2, 0u);
}

void font_done(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(2, 0u);
}

void font_set_alt(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(2, 0u);
}

void create_mbx(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    std::string name = "mbx";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_mbx_uid++;
    MbxRecord record{};
    record.name = std::move(name);
    g_mbxs.emplace(uid, std::move(record));
    if (thread_diag()) {
        std::cerr << "[tow1] create mbx uid=" << uid << " name=\""
                  << g_mbxs[uid].name << "\"\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void send_mbx(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t message = ctx.gpr[5];
    const auto mbx = g_mbxs.find(uid);
    if (mbx == g_mbxs.end()) {
        ctx.set_gpr(2, 0x8002019Bu);
        return;
    }
    if (!mbx->second.waiters.empty()) {
        MbxWaiter waiter = mbx->second.waiters.front();
        mbx->second.waiters.erase(mbx->second.waiters.begin());
        if (waiter.output_pointer != 0u &&
            runtime.memory().contains(waiter.output_pointer, 4u)) {
            runtime.memory().store32(waiter.output_pointer, message);
        }
        psprecomp::AllegrexContext context = waiter.context;
        context.set_gpr(2, 0u);
        enqueue_thread(waiter.uid, context);
    } else {
        mbx->second.messages.push_back(message);
    }
    ctx.set_gpr(2, 0u);
}

void receive_mbx(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t output_pointer = ctx.gpr[5];
    const auto mbx = g_mbxs.find(uid);
    if (mbx == g_mbxs.end()) {
        ctx.set_gpr(2, 0x8002019Bu);
        return;
    }
    if (!mbx->second.messages.empty()) {
        const std::uint32_t message = mbx->second.messages.front();
        mbx->second.messages.pop_front();
        if (output_pointer != 0u && runtime.memory().contains(output_pointer, 4u)) {
            runtime.memory().store32(output_pointer, message);
        }
        ctx.set_gpr(2, 0u);
        return;
    }

    const std::uint32_t timeout_pointer = ctx.gpr[6];
    if (timeout_pointer != 0u && runtime.memory().contains(timeout_pointer, 4u) &&
        runtime.memory().load32(timeout_pointer) == 0u) {
        ctx.set_gpr(2, 0x800201A8u); // MBOX_EMPTY for a zero timeout
        return;
    }

    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Waiting;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    mbx->second.waiters.push_back(MbxWaiter{g_scheduler.current_uid, output_pointer,
                                            suspended});
    if (!activate_next_thread(ctx, "receive-mbx")) {
        runtime.stop("PSP scheduler deadlock waiting for mailbox " + std::to_string(uid));
    }
}

void poll_mbx(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t output_pointer = ctx.gpr[5];
    const auto mbx = g_mbxs.find(uid);
    if (mbx == g_mbxs.end()) {
        ctx.set_gpr(2, 0x8002019Bu);
        return;
    }
    if (mbx->second.messages.empty()) {
        ctx.set_gpr(2, 0x800201A8u);
        return;
    }
    const std::uint32_t message = mbx->second.messages.front();
    mbx->second.messages.pop_front();
    if (output_pointer != 0u && runtime.memory().contains(output_pointer, 4u)) {
        runtime.memory().store32(output_pointer, message);
    }
    ctx.set_gpr(2, 0u);
}

void delete_mbx(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto mbx = g_mbxs.find(uid);
    if (mbx == g_mbxs.end()) {
        ctx.set_gpr(2, 0x8002019Bu);
        return;
    }
    for (MbxWaiter &waiter : mbx->second.waiters) {
        psprecomp::AllegrexContext context = waiter.context;
        context.set_gpr(2, 0x800201A7u);
        enqueue_thread(waiter.uid, context);
    }
    g_mbxs.erase(mbx);
    ctx.set_gpr(2, 0u);
}

void create_mutex(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    const std::int32_t initial = static_cast<std::int32_t>(ctx.gpr[6]);
    if (initial < 0) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    std::string name = "mutex";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_mutex_uid++;
    MutexRecord record{};
    record.name = std::move(name);
    // ToW1's MemoryMutex is created with initCount=1 and then locked/unlocked by
    // the main thread as if 1 were an available count rather than ownership.
    // Treating it as pre-owned deadlocks ModuleDispatcher. Keep this profile
    // default; PSPRECOMP_TOW1_OWNED_MUTEX=1 restores strict PSP ownership.
    const bool unowned_on_create =
        std::getenv("PSPRECOMP_TOW1_OWNED_MUTEX") == nullptr;
    record.count = unowned_on_create ? 0 : initial;
    record.owner = (!unowned_on_create && initial > 0)
        ? g_scheduler.current_uid : -1;
    g_mutexes.emplace(uid, std::move(record));
    if (thread_diag()) {
        std::cerr << "[tow1] create mutex uid=" << uid << " name=\""
                  << g_mutexes[uid].name << "\" count=" << initial << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void lock_mutex(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto mutex = g_mutexes.find(uid);
    if (mutex == g_mutexes.end() || count <= 0) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    if (mutex->second.owner == -1 || mutex->second.owner == g_scheduler.current_uid) {
        mutex->second.owner = g_scheduler.current_uid;
        mutex->second.count += count;
        ctx.set_gpr(2, 0u);
        return;
    }

    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Waiting;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    mutex->second.waiters.push_back(
        MutexWaiter{g_scheduler.current_uid, count, suspended});
    if (!activate_next_thread(ctx, "lock-mutex")) {
        runtime.stop("PSP scheduler deadlock waiting for mutex " + std::to_string(uid));
    }
}

void unlock_mutex(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto mutex = g_mutexes.find(uid);
    if (mutex == g_mutexes.end() || count <= 0 ||
        mutex->second.owner != g_scheduler.current_uid) {
        ctx.set_gpr(2, 0x800201A3u); // NOT_MUTEX_OWNER
        return;
    }
    mutex->second.count -= count;
    if (mutex->second.count <= 0) {
        mutex->second.count = 0;
        mutex->second.owner = -1;
        if (!mutex->second.waiters.empty()) {
            MutexWaiter waiter = mutex->second.waiters.front();
            mutex->second.waiters.erase(mutex->second.waiters.begin());
            mutex->second.owner = waiter.uid;
            mutex->second.count = waiter.count;
            psprecomp::AllegrexContext context = waiter.context;
            context.set_gpr(2, 0u);
            enqueue_thread(waiter.uid, context);
        }
    }
    ctx.set_gpr(2, 0u);
}

void delete_mutex(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto mutex = g_mutexes.find(uid);
    if (mutex == g_mutexes.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    for (MutexWaiter &waiter : mutex->second.waiters) {
        psprecomp::AllegrexContext context = waiter.context;
        context.set_gpr(2, 0x800201A7u); // WAIT_DELETE
        enqueue_thread(waiter.uid, context);
    }
    g_mutexes.erase(mutex);
    ctx.set_gpr(2, 0u);
}

void create_callback(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    const std::uint32_t function = ctx.gpr[6];
    const std::uint32_t argument = ctx.gpr[7];
    // A null function is legal: the object exists but notification is a no-op.
    // ToW1 creates such a callback as a boot-time checkpoint.
    std::string name = "callback";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_callback_uid++;
    g_callbacks.emplace(uid, CallbackRecord{std::move(name), function, argument,
                                            g_scheduler.current_uid});
    if (thread_diag()) {
        std::cerr << "[tow1] create callback uid=" << uid << " name=\""
                  << g_callbacks[uid].name << "\" function="
                  << psprecomp::hex32(function) << " arg="
                  << psprecomp::hex32(argument) << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void delete_callback(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    ctx.set_gpr(2, g_callbacks.erase(uid) == 1u ? 0u : 0x80020198u);
}

void create_semaphore(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_pointer = ctx.gpr[4];
    const std::int32_t initial = static_cast<std::int32_t>(ctx.gpr[6]);
    const std::int32_t maximum = static_cast<std::int32_t>(ctx.gpr[7]);
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    std::string name = "sema";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_semaphore_uid++;
    if (name == "SleepSema") g_sleep_semaphore_uid = uid;
    g_semaphores.emplace(uid, SemaphoreRecord{std::move(name), initial, maximum, {}});
    if (thread_diag()) {
        std::cerr << "[tow1] create sema uid=" << uid << " name=\""
                  << g_semaphores[uid].name << "\" value=" << initial
                  << " max=" << maximum << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
}

void wait_semaphore(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto semaphore = g_semaphores.find(uid);
    if (semaphore == g_semaphores.end() || count <= 0 || count > semaphore->second.maximum) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    if (semaphore->second.value >= count) {
        semaphore->second.value -= count;
        ctx.set_gpr(2, 0u);
        return;
    }

    const std::uint32_t timeout_pointer = ctx.gpr[6];
    if (timeout_pointer != 0u && runtime.memory().contains(timeout_pointer, 4u) &&
        runtime.memory().load32(timeout_pointer) == 0u) {
        ctx.set_gpr(2, 0x800201A8u); // SEMA_ZERO for a zero timeout
        return;
    }

    auto current = g_scheduler.threads.find(g_scheduler.current_uid);
    if (current == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u);
        return;
    }
    current->second.state = ThreadState::Waiting;
    psprecomp::AllegrexContext suspended = ctx;
    suspended.set_gpr(2, 0u);
    suspended.pc = ctx.gpr[31];
    current->second.context = suspended;
    semaphore->second.waiters.push_back(
        SemaphoreWaiter{g_scheduler.current_uid, count, suspended});

    if (!activate_next_thread(ctx, "wait-sema")) {
        runtime.stop("PSP scheduler deadlock waiting for semaphore " + std::to_string(uid));
    }
}

void signal_semaphore(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    (void)runtime;
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto semaphore = g_semaphores.find(uid);
    if (semaphore == g_semaphores.end() || count <= 0) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    semaphore->second.value = std::min(semaphore->second.maximum,
                                       semaphore->second.value + count);

    auto waiter = semaphore->second.waiters.begin();
    while (waiter != semaphore->second.waiters.end()) {
        if (semaphore->second.value < waiter->count) {
            ++waiter;
            continue;
        }
        semaphore->second.value -= waiter->count;
        psprecomp::AllegrexContext context = waiter->context;
        context.set_gpr(2, 0u);
        enqueue_thread(waiter->uid, context);
        waiter = semaphore->second.waiters.erase(waiter);
    }
    ctx.set_gpr(2, 0u);
}

void poll_semaphore(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto semaphore = g_semaphores.find(uid);
    if (semaphore == g_semaphores.end() || count <= 0) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    if (semaphore->second.value < count) {
        ctx.set_gpr(2, 0x800201A8u);
        return;
    }
    semaphore->second.value -= count;
    ctx.set_gpr(2, 0u);
}

void delete_semaphore(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto semaphore = g_semaphores.find(uid);
    if (semaphore == g_semaphores.end()) {
        ctx.set_gpr(2, 0x80020199u);
        return;
    }
    for (SemaphoreWaiter &waiter : semaphore->second.waiters) {
        psprecomp::AllegrexContext context = waiter.context;
        context.set_gpr(2, 0x800201A7u); // WAIT_DELETE
        enqueue_thread(waiter.uid, context);
    }
    g_semaphores.erase(semaphore);
    ctx.set_gpr(2, 0u);
}

std::uint64_t timeslice_interval() {
    // The startup/title state machine drives the module command queue through
    // paired getter/process calls on both user_main and ModuleDispatcher.  A
    // timer preemption between those pairs leaves the queue in its transient
    // processing state and stalls the title transition.  Keep the scheduler
    // cooperative by default; explicit sleeps and waits still switch threads.
    // PSPRECOMP_TOW1_TIMESLICE re-enables execution-driven preemption.
    const char *text = std::getenv("PSPRECOMP_TOW1_TIMESLICE");
    if (text == nullptr || *text == '\0') return 0u;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    return end != text && *end == '\0' ? value : 0u;
}

void initialize_scheduler(psprecomp::Runtime &runtime) {
    g_scheduler = Scheduler{};
    g_scheduler.next_stack_top = kUserRamTop - kModuleStackSize;
    g_sleep_semaphore_uid = -1;
    g_next_vblank_us = 16683u;

    ThreadRecord module{};
    module.name = "module_start";
    module.priority = 32u;
    module.stack_size = kModuleStackSize;
    module.stack_top = kUserRamTop;
    module.stack_bottom = kUserRamTop - kModuleStackSize;
    module.kernel_context = kUserRamTop - 0x100u;
    module.state = ThreadState::Running;
    module.context = runtime.cpu();
    g_scheduler.threads.emplace(0, std::move(module));
    g_scheduler.current_uid = 0;

    psprecomp::set_runtime_thread_identity(0, "module_start");
    runtime.register_function(kThreadReturnPc, &thread_return_trampoline,
                              "tow1_thread_return");
}

const char *thread_state_name(ThreadState state) {
    switch (state) {
    case ThreadState::Created: return "created";
    case ThreadState::Ready: return "ready";
    case ThreadState::Running: return "running";
    case ThreadState::Delayed: return "delayed";
    case ThreadState::Waiting: return "waiting";
    case ThreadState::Sleeping: return "sleeping";
    case ThreadState::Completed: return "completed";
    }
    return "?";
}

} // namespace

void report_threads() {
    if (g_mpeg_player)
        std::cerr << "[tow1] mpeg decoded_frames=" << g_mpeg_player->decoded_frames()
                  << " avc_calls=" << g_avc_decode_calls << "\n";
    std::cerr << "[tow1] thread states:\n";
    for (const auto &[uid, thread] : g_scheduler.threads) {
        std::cerr << "  uid=" << uid << " name=\"" << thread.name
                  << "\" state=" << thread_state_name(thread.state)
                  << " priority=" << thread.priority
                  << " pc=" << psprecomp::hex32(thread.context.pc)
                  << " ra=" << psprecomp::hex32(thread.context.gpr[31])
                  << " sp=" << psprecomp::hex32(thread.context.gpr[29])
                  << " s0=" << psprecomp::hex32(thread.context.gpr[16])
                  << " s1=" << psprecomp::hex32(thread.context.gpr[17])
                  << " s2=" << psprecomp::hex32(thread.context.gpr[18])
                  << " s3=" << psprecomp::hex32(thread.context.gpr[19]) << "\n";
    }
    std::cerr << "[tow1] semaphores:\n";
    for (const auto &[uid, semaphore] : g_semaphores) {
        std::cerr << "  uid=" << uid << " name=\"" << semaphore.name
                  << "\" value=" << semaphore.value
                  << " waiters=" << semaphore.waiters.size() << "\n";
    }
    std::cerr << "[tow1] mutexes:\n";
    for (const auto &[uid, mutex] : g_mutexes) {
        std::cerr << "  uid=" << uid << " name=\"" << mutex.name
                  << "\" owner=" << mutex.owner << " count=" << mutex.count
                  << " waiters=" << mutex.waiters.size() << "\n";
    }
    std::cerr << "[tow1] mailboxes:\n";
    for (const auto &[uid, mbx] : g_mbxs) {
        std::cerr << "  uid=" << uid << " name=\"" << mbx.name
                  << "\" messages=" << mbx.messages.size()
                  << " waiters=" << mbx.waiters.size() << "\n";
    }
    std::cerr << "[tow1] open files:\n";
    for (const auto &[fd, file] : g_files) {
        std::cerr << "  fd=" << fd << " pos=" << file.position
                  << " size=" << file.size << " path=\"" << file.psp_path << "\"\n";
    }
}

void display_wait_tick() {
    if (std::getenv("PSPRECOMP_TOW1_NO_WAIT_TIME") == nullptr) {
        g_scheduler.now_us += 16683u;
        ++g_vblank_wait_generation;
    }
}

void register_hle(psprecomp::Runtime &runtime, std::uint32_t user_arena_start) {
    g_partitions = PartitionTable{};
    g_partitions.next_address = (user_arena_start + 0xFFu) & ~0xFFu;
    g_module_end = user_arena_start;
    ge_reset();
    initialize_scheduler(runtime);
    if (heap_diag()) {
        psprecomp::set_runtime_pre_chained_call_hook(heap_pre_chained);
        psprecomp::set_runtime_post_chained_call_hook(heap_post_chained);
    }

    if (std::getenv("PSPRECOMP_TOW1_NULLQ_DIAG") != nullptr) {
        psprecomp::set_runtime_pre_chained_call_hook(
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
               std::uint32_t target_pc, std::uint32_t depth) {
                if (target_pc != 0x08B17F8Cu || ctx.gpr[4] != 0u) return;
                const std::uint32_t recovered = rt.memory().load32(0x08D42224u);
                std::cerr << "[tow1-nullq] uid=" << g_scheduler.current_uid
                          << " depth=" << depth
                          << " pc=" << psprecomp::hex32(ctx.pc)
                          << " ra=" << psprecomp::hex32(ctx.gpr[31])
                          << " global=" << psprecomp::hex32(recovered)
                          << " title=" << psprecomp::hex32(rt.memory().load32(0x08DB61D0u))
                          << "\n";
                if (recovered != 0u) ctx.gpr[4] = recovered;
            });
    }
    if (std::getenv("PSPRECOMP_TOW1_PPT_SUBOBJ") != nullptr) {
        psprecomp::set_runtime_pre_dispatch_hook(
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx,
               std::uint32_t dispatch_pc, std::int32_t) {
                if (dispatch_pc == 0x08B0F7B0u && g_last_ppt_object != 0u) {
                    std::cerr << "[tow1-ppt-subobj-dispatch] object="
                              << psprecomp::hex32(g_last_ppt_object)
                              << " old_a0=" << psprecomp::hex32(ctx.gpr[4]) << "\n";
                    ctx.gpr[4] = g_last_ppt_object + 4u;
                }
            });
    }
    if (std::getenv("PSPRECOMP_TOW1_PPT_UPLOAD") != nullptr) {
        psprecomp::set_runtime_pre_dispatch_hook(
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &,
               std::uint32_t, std::int32_t) {
                static std::uint64_t ticks = 0u;
                static bool uploaded = false;
                if (uploaded) return;
                if ((++ticks % 5'000'000u) != 0u) return;
                auto &memory = rt.memory();
                for (std::uint32_t at = 0x08000000u; at < 0x0A000000u; at += 4u) {
                    if (memory.load32(at) != 0x49425A45u) continue;
                    if (memory.load32(at + 4u) != 0x0000444Eu) continue;
                    if (memory.load32(at + 0x110u) != 0x5F474E42u ||
                        memory.load32(at + 0x114u) != 0x5F726F66u)
                        continue;
                    if (upload_ppt_data(rt, at + 0x200u, "BNG")) uploaded = true;
                    return;
                }
            });
    }
    if (std::getenv("PSPRECOMP_TOW1_PPT_DIAG") != nullptr) {
        psprecomp::set_runtime_pre_chained_call_hook(
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
               std::uint32_t target_pc, std::uint32_t) {
                if (target_pc == 0x08AC5B70u) {
                    std::string path;
                    const std::uint32_t ptr = ctx.gpr[5];
                    if (ptr != 0u && rt.memory().contains(ptr, 1u))
                        path = rt.memory().read_c_string(ptr, 128u);
                    std::cerr << "[tow1-ppt] open \"" << path << "\"\n";
                } else if (target_pc == 0x08B0F7B0u) {
                    const std::uint32_t obj = ctx.gpr[4];
                    std::cerr << "[tow1-render] FUN_08B0F7B0 a0=" << psprecomp::hex32(obj)
                              << " [a0]=" << psprecomp::hex32(rt.memory().load32(obj))
                              << " [a0+4]=" << psprecomp::hex32(rt.memory().load32(obj + 4u))
                              << " [a0+20]=" << psprecomp::hex32(rt.memory().load32(obj + 0x20u))
                              << "\n";
                    if (std::getenv("PSPRECOMP_TOW1_PPT_SUBOBJ") != nullptr &&
                        g_last_ppt_object != 0u) {
                        std::cerr << "[tow1-ppt-subobj] object="
                                  << psprecomp::hex32(g_last_ppt_object)
                                  << " old_a0=" << psprecomp::hex32(ctx.gpr[4]) << "\n";
                        ctx.gpr[4] = g_last_ppt_object + 4u;
                    }
                } else if (target_pc == 0x08B0C7A4u) {
                    const std::uint32_t obj = ctx.gpr[4];
                    std::cerr << "[tow1-render] FUN_08B0C7A4 a0=" << psprecomp::hex32(obj)
                              << " [a0]=" << psprecomp::hex32(rt.memory().load32(obj)) << "\n";
                } else if (target_pc == 0x08B0CA9Cu) {
                    if (std::getenv("PSPRECOMP_TOW1_PPT_SWAP") != nullptr) {
                        const std::uint32_t a0 = ctx.gpr[4];
                        const std::uint32_t a1 = ctx.gpr[5];
                        if (a0 != 0u && a1 != 0u &&
                            rt.memory().contains(a1 + 0x48u, 4u)) {
                            const std::uint32_t w = rt.memory().load32(a1 + 0x40u);
                            const std::uint32_t h = rt.memory().load32(a1 + 0x44u);
                            if ((w == 480u || w == 512u) && (h == 270u || h == 272u)) {
                                ctx.gpr[4] = a1;
                                ctx.gpr[5] = a0;
                            }
                        }
                    }
                    const std::uint32_t file = ctx.gpr[4];
                    if (std::getenv("PSPRECOMP_TOW1_PPT_INIT") != nullptr &&
                        file != 0u && rt.memory().contains(file + 0x28u, 4u)) {
                        const std::uint32_t texture = rt.memory().load32(file + 0x20u);
                        const std::uint32_t palette = rt.memory().load32(file + 0x24u);
                        if (palette == 0u && texture != 0u) {
                            rt.memory().store32(file + 0x24u, texture);
                            std::cerr << "[tow1-ppt-init] initialized palette slot on "
                                      << psprecomp::hex32(file)
                                      << " -> " << psprecomp::hex32(texture) << "\n";
                        }
                    }
                    if (std::getenv("PSPRECOMP_TOW1_PPT_UPLOAD") != nullptr)
                        upload_title_ppt(rt, file, "BNG");
                    std::cerr << "[tow1-ppt] upload file=" << psprecomp::hex32(file)
                              << " sprite=" << psprecomp::hex32(ctx.gpr[5]);
                    if (file != 0u && rt.memory().contains(file + 0x44u, 8u)) {
                        std::cerr << " vt=" << psprecomp::hex32(rt.memory().load32(file))
                                  << " w=" << psprecomp::hex32(rt.memory().load32(file + 0x40u))
                                  << " h=" << psprecomp::hex32(rt.memory().load32(file + 0x44u))
                                  << " +0x10=" << psprecomp::hex32(rt.memory().load32(file + 0x10u))
                                  << " +0x14=" << psprecomp::hex32(rt.memory().load32(file + 0x14u))
                                  << " +0x20=" << psprecomp::hex32(rt.memory().load32(file + 0x20u))
                                  << " +0x24=" << psprecomp::hex32(rt.memory().load32(file + 0x24u));
                    }
                    std::cerr << "\n";
                } else if (target_pc == 0x08B0F57Cu) {
                    const std::uint32_t object = ctx.gpr[4];
                    g_last_ppt_object = object;
                    if (std::getenv("PSPRECOMP_TOW1_PPT_SELF") != nullptr &&
                        object != 0u && rt.memory().contains(object + 0x18u, 4u)) {
                        const std::uint32_t w = rt.memory().load32(object + 0x10u);
                        const std::uint32_t h = rt.memory().load32(object + 0x14u);
                        if (w == 480u && (h == 270u || h == 272u))
                            rt.memory().store32(object + 4u, object);
                    }
                    std::cerr << "[tow1-ppt] load object=" << psprecomp::hex32(object);
                    if (object != 0u && rt.memory().contains(object, 0x20u)) {
                        const std::uint32_t file = rt.memory().load32(object);
                        std::cerr << " file=" << psprecomp::hex32(file);
                        if (file != 0u && rt.memory().contains(file + 0xB0u, 8u)) {
                            std::cerr << " loadfn=" << psprecomp::hex32(rt.memory().load32(file + 0x60u))
                                      << " uploadfn=" << psprecomp::hex32(rt.memory().load32(file + 0xB0u));
                        }
                        std::cerr << "\n[tow1-ppt]   object:";
                        for (std::uint32_t i = 0u; i < 0x100u; i += 4u)
                            std::cerr << " " << psprecomp::hex32(rt.memory().load32(object + i));
                        if (file != 0u && rt.memory().contains(file, 0x100u)) {
                            std::cerr << "\n[tow1-ppt]   fileobj:";
                            for (std::uint32_t i = 0u; i < 0x100u; i += 4u)
                                std::cerr << " " << psprecomp::hex32(rt.memory().load32(file + i));
                        }
                    }
                    std::cerr << "\n";
                }
            });
        psprecomp::set_runtime_post_chained_call_hook(
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
               std::uint32_t target_pc, std::uint32_t) {
                if (target_pc == 0x08AC5B70u) {
                    const std::uint32_t file = ctx.gpr[2];
                    std::cerr << "[tow1-ppt] file=" << psprecomp::hex32(file);
                    if (file != 0u) {
                        std::cerr << " vt=" << psprecomp::hex32(rt.memory().load32(file))
                                  << " +4=" << psprecomp::hex32(rt.memory().load32(file + 4u))
                                  << " +8=" << psprecomp::hex32(rt.memory().load32(file + 8u))
                                  << " +C=" << psprecomp::hex32(rt.memory().load32(file + 0xCu));
                        const std::uint32_t sub = rt.memory().load32(file + 4u);
                        if (sub != 0u && rt.memory().contains(sub, 0x40u)) {
                            std::cerr << "\n[tow1-ppt]   sub:";
                            for (std::uint32_t i = 0u; i < 0x40u; i += 4u)
                                std::cerr << " " << psprecomp::hex32(rt.memory().load32(sub + i));
                        }
                    }
                    std::cerr << "\n";
                } else if (target_pc == 0x08B0F7B0u) {
                    std::cerr << "[tow1-render] FUN_08B0F7B0 ret=" << psprecomp::hex32(ctx.gpr[2])
                              << " rect=" << psprecomp::hex32(rt.memory().load32(0x000E9480u))
                              << "," << psprecomp::hex32(rt.memory().load32(0x000E9484u))
                              << " title=" << psprecomp::hex32(rt.memory().load32(0x08DB61D0u))
                              << "\n";
                } else if (target_pc == 0x08B0F57Cu) {
                    if (std::getenv("PSPRECOMP_TOW1_PPT_UPLOAD") != nullptr &&
                        g_last_ppt_object != 0u)
                        upload_title_ppt(rt, g_last_ppt_object, "BNG");
                    const std::uint32_t texture = ctx.gpr[2];
                    std::cerr << "[tow1-ppt] loader result=" << psprecomp::hex32(texture)
                              << " file=" << psprecomp::hex32(ctx.gpr[4]);
                    if (texture != 0u && rt.memory().contains(texture, 0x40u)) {
                        std::cerr << "\n[tow1-ppt]   texture:";
                        for (std::uint32_t i = 0u; i < 0x40u; i += 4u)
                            std::cerr << " " << psprecomp::hex32(rt.memory().load32(texture + i));
                    }
                    std::cerr << "\n";
                }
            });
    }
    if (std::getenv("PSPRECOMP_TOW1_BRANCH_DIAG") != nullptr) {
        // Diagnostic for the title's init gate: FUN_08b1f12c returns
        // (*(0x08E0E3C8) >= 2).  Log the value the guest actually observes.
        psprecomp::set_runtime_post_chained_call_hook(
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
               std::uint32_t target_pc, std::uint32_t depth) {
                if (target_pc != 0x08B1F12Cu) return;
                static std::uint32_t logged = 0u;
                if (logged >= 24u) return;
                ++logged;
                const std::uint32_t state = rt.memory().load32(0x08E0E3C8u);
                std::cerr << "[tow1-branch] depth=" << depth
                          << " pc=" << psprecomp::hex32(ctx.pc)
                          << " state=" << psprecomp::hex32(state)
                          << " ret=" << ctx.gpr[2] << "\n";
            });
    }

    runtime.register_hle("SysMemUserForUser", 0x7591C7DBu, // sceKernelSetCompiledSdkVersion
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("SysMemUserForUser", 0xF77D77CBu, // sceKernelSetCompilerVersion
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("SysMemUserForUser", 0x13A5ABEFu, // sceKernelPrintf
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t format_pointer = ctx.gpr[4];
            if (format_pointer != 0u && rt.memory().contains(format_pointer, 1u)) {
                std::cerr << "[guest printf] "
                          << rt.memory().read_c_string(format_pointer, 512u) << "\n";
            }
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("SysMemUserForUser", 0x237DBD4Fu, // sceKernelAllocPartitionMemory
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t name_pointer = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[7];
            constexpr std::uint32_t alignment = 0x100u;
            const std::uint32_t aligned_size = (size + alignment - 1u) & ~(alignment - 1u);
            const std::uint32_t address =
                (g_partitions.next_address + alignment - 1u) & ~(alignment - 1u);
            if (aligned_size == 0u ||
                !rt.memory().contains(address, aligned_size) ||
                address + aligned_size > g_scheduler.next_stack_top) {
                ctx.set_gpr(2, 0x80020190u); // NO_MEMORY
                return;
            }
            std::string name = "partition";
            if (name_pointer != 0u && rt.memory().contains(name_pointer, 1u)) {
                name = rt.memory().read_c_string(name_pointer, 128u);
            }
            rt.memory().zero(address, aligned_size);
            if (heap_diag())
                dump_heap(rt, "partition-alloc", kGameHeap, aligned_size);
            const std::int32_t uid = g_partitions.next_uid++;
            g_partitions.blocks.emplace(uid, PartitionBlock{name, address, aligned_size});
            g_partitions.next_address = address + aligned_size;
            if (thread_diag() || std::getenv("PSPRECOMP_PARTITION_DIAG") != nullptr) {
                std::cerr << "[tow1] partition alloc uid=" << uid << " name=\"" << name
                          << "\" addr=" << psprecomp::hex32(address)
                          << " size=" << psprecomp::hex32(aligned_size)
                          << " next=" << psprecomp::hex32(g_partitions.next_address)
                          << " stack_top=" << psprecomp::hex32(g_scheduler.next_stack_top)
                          << "\n";
            }
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("SysMemUserForUser", 0x9D9A5BA1u, // sceKernelGetBlockHeadAddr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto block = g_partitions.blocks.find(uid);
            ctx.set_gpr(2, block == g_partitions.blocks.end() ? 0u : block->second.address);
        });
    runtime.register_hle("SysMemUserForUser", 0xB6D61D02u, // sceKernelFreePartitionMemory
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            ctx.set_gpr(2, g_partitions.blocks.erase(uid) == 1u ? 0u : 0x800200CBu);
        });
    runtime.register_hle("SysMemUserForUser", 0xA291F107u, // sceKernelMaxFreeMemSize
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t low = (g_partitions.next_address + 0xFFu) & ~0xFFu;
            const std::uint32_t high = g_scheduler.next_stack_top & ~0xFFu;
            ctx.set_gpr(2, high > low ? high - low : 0u);
        });

    // Coherent host memory makes cache maintenance a no-op.
    for (const std::uint32_t nid : {0x79D1C3FAu, 0xB435DEC5u, 0xBFA98062u,
                                    0x34B9FA9Eu, 0x3EE30821u}) {
        runtime.register_hle("UtilsForUser", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2, 0u);
            });
    }
    runtime.register_hle("UtilsForUser", 0x91E4F6A7u, // sceKernelLibcClock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_scheduler.now_us));
        });
    runtime.register_hle("UtilsForUser", 0x27CC57F0u, // sceKernelLibcTime
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t seconds =
                static_cast<std::uint32_t>(g_scheduler.now_us / 1000000u);
            if (ctx.gpr[4] != 0u && rt.memory().contains(ctx.gpr[4], 4u))
                rt.memory().store32(ctx.gpr[4], seconds);
            ctx.set_gpr(2, seconds);
        });
    runtime.register_hle("UtilsForUser", 0x71EC4271u, // sceKernelLibcGettimeofday
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t seconds =
                static_cast<std::uint32_t>(g_scheduler.now_us / 1000000u);
            const std::uint32_t microseconds =
                static_cast<std::uint32_t>(g_scheduler.now_us % 1000000u);
            if (ctx.gpr[4] != 0u && rt.memory().contains(ctx.gpr[4], 8u)) {
                rt.memory().store32(ctx.gpr[4], seconds);
                rt.memory().store32(ctx.gpr[4] + 4u, microseconds);
            }
            ctx.set_gpr(2, 0u);
        });

    // Silent audio layer: channel bookkeeping only, no host output yet.
    runtime.register_hle("sceAudio", 0x5EC81C55u, // sceAudioChReserve
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::int32_t requested = static_cast<std::int32_t>(ctx.gpr[4]);
            std::int32_t channel = requested;
            if (channel < 0) {
                channel = -1;
                for (std::int32_t candidate = 0; candidate < 8; ++candidate) {
                    if (!g_audio_channel_reserved[static_cast<std::size_t>(candidate)]) {
                        channel = candidate;
                        break;
                    }
                }
            }
            if (channel < 0 || channel >= 8 ||
                g_audio_channel_reserved[static_cast<std::size_t>(channel)]) {
                ctx.set_gpr(2, 0x80260001u);
                return;
            }
            g_audio_channel_reserved[static_cast<std::size_t>(channel)] = true;
            g_audio_channel_samples[static_cast<std::size_t>(channel)] = ctx.gpr[5];
            ctx.set_gpr(2, static_cast<std::uint32_t>(channel));
        });
    runtime.register_hle("sceAudio", 0x6FC46853u, // sceAudioChRelease
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t channel = ctx.gpr[4];
            if (channel < 8u) {
                g_audio_channel_reserved[channel] = false;
                g_audio_channel_samples[channel] = 0u;
            }
            ctx.set_gpr(2, 0u);
        });
    auto audio_block = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t channel = ctx.gpr[4];
        const std::uint32_t samples =
            channel < 8u ? g_audio_channel_samples[channel] : 0u;
        const std::uint32_t rate =
            g_sas.sample_rate > 0 ? static_cast<std::uint32_t>(g_sas.sample_rate)
                                  : 44100u;
        const std::uint32_t microseconds =
            samples != 0u ? (samples * 1000000u) / rate : 16683u;

        auto current = g_scheduler.threads.find(g_scheduler.current_uid);
        if (current == g_scheduler.threads.end()) {
            ctx.set_gpr(2, 0u);
            return;
        }
        current->second.state = ThreadState::Delayed;
        current->second.delay_until_us = g_scheduler.now_us + microseconds;
        current->second.delay_sequence = g_scheduler.next_sequence++;
        psprecomp::AllegrexContext suspended = ctx;
        suspended.set_gpr(2, 0u);
        suspended.pc = ctx.gpr[31];
        current->second.context = suspended;
        if (!activate_next_thread(ctx, "audio-block")) {
            g_scheduler.now_us = current->second.delay_until_us;
            current->second.state = ThreadState::Running;
            ctx.set_gpr(2, 0u);
        }
    };
    runtime.register_hle("sceAudio", 0x136CAF51u, audio_block); // OutputBlocking
    runtime.register_hle("sceAudio", 0x13F592BCu, audio_block); // OutputPannedBlocking
    runtime.register_hle("sceAudio", 0xE2D56B2Du, // OutputPanned (non-blocking)
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    for (const std::uint32_t nid : {0x95FD0C2Du, 0xB7E1D8E7u, 0xCB2E439Eu,
                                    0xB011922Fu}) {
        runtime.register_hle("sceAudio", nid, // config / volume / length queries
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2, 0u);
            });
    }

    // Silent sceSasCore. The core object and its grain/output mode are tracked
    // so the guest's output buffer is the right size; samples are zeroed.
    runtime.register_hle("sceSasCore", 0x42778A9Fu, // __sceSasInit
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_sas.core = ctx.gpr[4];
            g_sas.grain = static_cast<std::int32_t>(ctx.gpr[5]);
            g_sas.max_voices = static_cast<std::int32_t>(ctx.gpr[6]);
            g_sas.output_mode = static_cast<std::int32_t>(ctx.gpr[7]);
            g_sas.sample_rate = static_cast<std::int32_t>(ctx.gpr[8]);
            ctx.set_gpr(2, 0u);
        });
    auto sas_output = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t output = ctx.gpr[5];
        if (output != 0u && g_sas.grain > 0) {
            const std::size_t bytes = static_cast<std::size_t>(g_sas.grain) *
                (g_sas.output_mode == 0 ? 2u : 4u);
            if (rt.memory().contains(output, bytes)) rt.memory().zero(output, bytes);
        }
        // Model the core's grain duration so this call blocks/yields instead of
        // letting the sound manager spin and starve the file/title threads.
        const std::uint32_t rate =
            g_sas.sample_rate > 0u ? static_cast<std::uint32_t>(g_sas.sample_rate) : 44100u;
        const std::uint32_t samples =
            g_sas.grain > 0 ? static_cast<std::uint32_t>(g_sas.grain) : 1024u;
        const std::uint32_t microseconds = std::max<std::uint32_t>(
            100u, (samples * 1000000u) / rate);
        delay_current_thread(rt, ctx, microseconds);
        ctx.set_gpr(2, 0u);
    };
    runtime.register_hle("sceSasCore", 0xA3589D81u, sas_output); // __sceSasCore
    runtime.register_hle("sceSasCore", 0x50A14DFCu, sas_output); // __sceSasCoreWithMix
    runtime.register_hle("sceSasCore", 0xBD11B7C2u, // __sceSasGetGrain
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_sas.grain));
        });
    runtime.register_hle("sceSasCore", 0xD1E0A01Eu, // __sceSasSetGrain
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_sas.grain = static_cast<std::int32_t>(ctx.gpr[5]);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceSasCore", 0xE175EF66u, // __sceSasGetOutputmode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_sas.output_mode));
        });
    runtime.register_hle("sceSasCore", 0xE855BF76u, // __sceSasSetOutputmode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_sas.output_mode = static_cast<std::int32_t>(ctx.gpr[5]);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceSasCore", 0x68A46B95u, // __sceSasGetEndFlag
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceSasCore", 0x2C8E6AB3u, // __sceSasGetPauseFlag
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceSasCore", 0x74AE582Au, // __sceSasGetEnvelopeHeight
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    for (const std::uint32_t nid : {0x019B25EBu, 0x267A6DD2u, 0x33D4AB37u,
                                    0x440CA7D8u, 0x5F9529F6u, 0x76F01ACAu,
                                    0x787D04D5u, 0x99944089u, 0x9EC3676Au,
                                    0xA0CF2FA4u, 0xAD84D37Fu, 0xB7660A23u,
                                    0xCBCD4F79u, 0xD5A229C9u, 0xF983B186u}) {
        runtime.register_hle("sceSasCore", nid, // voice/ADSR/volume/pause setters
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2, 0u);
            });
    }

    runtime.register_hle("sceWlanDrv", 0x0C622081u, // sceWlanGetEtherAddr
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            if (address != 0u && rt.memory().contains(address, 6u)) {
                const std::uint8_t mac[6] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
                rt.memory().copy_in(address, mac);
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceWlanDrv", 0xD7763699u, // sceWlanGetSwitchState
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("sceCtrl", 0x1F4011E6u, // sceCtrlSetSamplingMode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceCtrl", 0x1F803938u, // sceCtrlReadBufferPositive
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t buffer = ctx.gpr[4];
            const std::uint32_t count = ctx.gpr[5];
            if (buffer == 0u || count == 0u ||
                !rt.memory().contains(buffer, static_cast<std::size_t>(count) * 16u)) {
                ctx.set_gpr(2, 0u);
                return;
            }
            std::uint32_t buttons = window_buttons();
            if (std::getenv("PSPRECOMP_TOW1_PRESS_START") != nullptr) {
                // Do not inject input during the opening movie.  The title
                // pointer becomes non-zero when the title state is active.
                const bool title_active = rt.memory().load32(0x08DB61D0u) != 0u;
                const bool start_only =
                    std::getenv("PSPRECOMP_TOW1_PRESS_START_ONLY") != nullptr;
                const bool title_ready =
                    g_scheduler.now_us >= 95000000u; // after the opening movie
                if (start_only) {
                    static bool start_seen = false;
                    static std::uint64_t start_seen_us = 0u;
                    if (title_ready && !start_seen) {
                        start_seen = true;
                        start_seen_us = g_scheduler.now_us;
                    }
                    if (start_seen) {
                        const std::uint64_t elapsed = g_scheduler.now_us - start_seen_us;
                        if (elapsed < 250000u ||
                            (elapsed >= 10000000u && elapsed < 10250000u))
                            buttons |= 0x0008u;
                        if (elapsed >= 15000000u && elapsed < 15250000u)
                            buttons |= 0x4000u; // New Game
                        if (const char *menu = std::getenv("PSPRECOMP_TOW1_MENU_BUTTON");
                            menu != nullptr && elapsed >= 20000000u && elapsed < 80000000u &&
                            (elapsed % 5000000u) < 250000u)
                            buttons |= static_cast<std::uint32_t>(
                                std::strtoul(menu, nullptr, 0));
                    }
                } else {
                    static bool sequence_armed = false;
                    static std::uint64_t title_seen_us = 0u;
                    static bool cross_sent = false;
                    if (title_ready && !sequence_armed) {
                        sequence_armed = true;
                        title_seen_us = g_scheduler.now_us;
                    }
                    if (sequence_armed && !cross_sent) {
                        const std::uint64_t elapsed = g_scheduler.now_us - title_seen_us;
                        if (elapsed >= 1000000u && elapsed < 1250000u)
                            buttons |= 0x4000u; // Cross: leave the title/intro
                        if (elapsed >= 1250000u) cross_sent = true;
                    }
                    if (sequence_armed && cross_sent) {
                        const std::uint64_t elapsed = g_scheduler.now_us - title_seen_us;
                        if (const char *menu = std::getenv("PSPRECOMP_TOW1_MENU_BUTTON");
                            menu != nullptr && elapsed >= 30000000u && elapsed < 180000000u &&
                            (elapsed % 5000000u) < 250000u)
                            buttons |= static_cast<std::uint32_t>(
                                std::strtoul(menu, nullptr, 0));
                    }
                }
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t at = buffer + index * 16u;
                rt.memory().zero(at, 16u);
                rt.memory().store32(at, static_cast<std::uint32_t>(g_scheduler.now_us));
                rt.memory().store32(at + 4u, buttons);
                // LX/LY/RX/RY stay at the neutral 128.
                rt.memory().store32(at + 8u, 0x80808080u);
                rt.memory().store32(at + 12u, 0u);
            }
            ctx.set_gpr(2, count);
        });

    runtime.register_hle("IoFileMgrForUser", 0x109F50BCu, // sceIoOpen
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_open(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x89AA9906u, // sceIoOpenAsync
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_open_async(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x810C4BC3u, // sceIoClose
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_close(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x6A638D83u, // sceIoRead
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_read(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xA0B5A7C2u, // sceIoReadAsync
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_read_async(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x35DBD746u, // sceIoWaitAsyncCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_wait_async_cb(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x27EB27B8u, // sceIoLseek
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_lseek(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xACE946E8u, // sceIoGetstat
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_getstat(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xB29DDF9Cu, // sceIoDopen
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_dopen(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xE3EB004Cu, // sceIoDread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_dread(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xEB092469u, // sceIoDclose
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_dclose(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x54F5FB11u, // sceIoDevctl
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_devctl(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x63632449u, // sceIoIoctl
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_ioctl(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x42EC03ACu, // sceIoWrite
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_write(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x779103A0u, // sceIoRename
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { io_rename(rt, ctx); });

    runtime.register_hle("ThreadManForUser", 0xA66B0120u, // sceKernelReferEventFlagStatus
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            refer_event_flag_status(rt, ctx);
        });
    runtime.register_hle("sceDmac", 0x617F3FE6u, // sceDmacMemcpy
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t destination = ctx.gpr[4];
            const std::uint32_t source = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] dma dst=" << psprecomp::hex32(destination)
                          << " src=" << psprecomp::hex32(source)
                          << " size=" << size << "\n";
            }
            if (size == 0u) {
                ctx.set_gpr(2, 0u);
                return;
            }
            if (!rt.memory().contains(source, size) ||
                !rt.memory().contains(destination, size)) {
                ctx.set_gpr(2, 0x80020005u);
                return;
            }
            std::vector<std::uint8_t> copy(size);
            rt.memory().copy_out(source, std::span<std::uint8_t>(copy.data(), copy.size()));
            rt.memory().copy_in(destination,
                                std::span<const std::uint8_t>(copy.data(), copy.size()));
            ctx.set_gpr(2, 0u);
        });

    // Minimal MPEG lifecycle contract. The title opens the opening-movie
    // stream during startup; decoding itself is not part of this profile yet.
    runtime.register_hle("sceMpeg", 0x682A619Bu, // sceMpegInit
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (mpeg_diag()) std::cerr << "[tow1-mpeg] init\n";
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("StdioForUser", 0x172D316Eu, // sceKernelStdout
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("StdioForUser", 0xA6BAB2E9u, // sceKernelStderr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 2u);
        });
    runtime.register_hle("StdioForUser", 0xBDBE0D60u, // sceKernelStdin
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x13407F13u, // sceMpegRingbufferDestruct
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x606A4649u, // sceMpegDelete
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (MpegState *state = mpeg_state_for(rt, ctx.gpr[4]); state != nullptr) {
                const std::uint32_t address = state->address;
                const std::uint32_t handle = state->work + 0x30u;
                g_mpeg_states.erase(address);
                g_mpeg_states.erase(handle);
            }
            if (g_mpeg_player) g_mpeg_player->close();
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x707B7629u, // sceMpegFlushAllStream
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x874624D6u, // sceMpegFinish
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (g_mpeg_player) g_mpeg_player->close();
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xD7A29F46u, // sceMpegRingbufferQueryMemSize
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // One MPEG packet is 2048 bytes plus the 104-byte descriptor.
            ctx.set_gpr(2, ctx.gpr[4] * 2152u);
        });
    runtime.register_hle("sceMpeg", 0xC132E22Fu, // sceMpegQueryMemSize
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // MPEG library 0x0105 and later require 64 KiB of work memory.
            ctx.set_gpr(2, 0x10000u);
        });
    runtime.register_hle("sceMpeg", 0x37295ED8u, // sceMpegRingbufferConstruct
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ringbuffer = ctx.gpr[4];
            const std::uint32_t packets = ctx.gpr[5];
            const std::uint32_t data = ctx.gpr[6];
            const std::uint32_t size = ctx.gpr[7];
            const std::uint32_t callback = ctx.gpr[8];
            const std::uint32_t callback_arg = ctx.gpr[9];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] ringbuffer-construct ring=" << psprecomp::hex32(ringbuffer)
                          << " packets=" << packets << " data=" << psprecomp::hex32(data)
                          << " size=" << size << " cb=" << psprecomp::hex32(callback)
                          << " arg=" << psprecomp::hex32(callback_arg) << "\n";
            }
            if (ringbuffer == 0u || !rt.memory().contains(ringbuffer, 48u)) {
                ctx.set_gpr(2, 0x80020005u);
                return;
            }
            if (size < packets * 2152u) {
                ctx.set_gpr(2, 0x80020005u);
                return;
            }
            rt.memory().zero(ringbuffer, 48u);
            rt.memory().store32(ringbuffer + 0u, packets);
            rt.memory().store32(ringbuffer + 4u, 0u);
            rt.memory().store32(ringbuffer + 8u, 0u);
            rt.memory().store32(ringbuffer + 12u, 0u);
            rt.memory().store32(ringbuffer + 16u, 2048u);
            rt.memory().store32(ringbuffer + 20u, data);
            rt.memory().store32(ringbuffer + 24u, callback);
            rt.memory().store32(ringbuffer + 28u, callback_arg);
            rt.memory().store32(ringbuffer + 32u, data + packets * 2048u);
            rt.memory().store32(ringbuffer + 36u, 0u);
            rt.memory().store32(ringbuffer + 40u, 0u);
            rt.memory().store32(ringbuffer + 44u, 0x08DBA950u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xD8C5F121u, // sceMpegCreate
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t mpeg_address = ctx.gpr[4];
            const std::uint32_t work = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            const std::uint32_t ringbuffer = ctx.gpr[7];
            const std::uint32_t frame_width = ctx.gpr[8];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] create mpeg=" << psprecomp::hex32(mpeg_address)
                          << " work=" << psprecomp::hex32(work) << " size=" << size
                          << " ring=" << psprecomp::hex32(ringbuffer)
                          << " width=" << frame_width << " mode=" << ctx.gpr[9]
                          << " ddr=" << ctx.gpr[10] << "\n";
            }
            if (mpeg_address == 0u || work == 0u || size < 0x10000u) {
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            const std::uint32_t handle = work + 0x30u;
            if (!rt.memory().contains(handle, 0x40u)) {
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            // The MPEG handle lives in the caller's work buffer.  Store it
            // back through the caller's handle slot so later calls can find it.
            rt.memory().store32(mpeg_address, handle);
            rt.memory().zero(handle, 0x40u);
            rt.memory().copy_in(handle, std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>("LIBMPEG\0"), 8u));
            rt.memory().copy_in(handle + 8u, std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>("001\0"), 4u));
            rt.memory().store32(handle + 12u, 0xFFFFFFFFu);
            rt.memory().store32(handle + 16u, ringbuffer);
            const std::uint32_t upper_bound =
                ringbuffer != 0u && rt.memory().contains(ringbuffer, 48u)
                    ? rt.memory().load32(ringbuffer + 32u) : 0u;
            rt.memory().store32(handle + 20u, upper_bound);
            g_mpeg_video_eof = false;
            MpegState state{};
            state.address = mpeg_address;
            state.work = work;
            state.ringbuffer = ringbuffer;
            state.frame_width = frame_width;
            g_mpeg_states[handle] = state;
            g_mpeg_states[mpeg_address] = state;
            if (ringbuffer != 0u && rt.memory().contains(ringbuffer, 48u))
                rt.memory().store32(ringbuffer + 40u, mpeg_address);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x21FF80E4u, // sceMpegQueryStreamOffset
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t buffer = ctx.gpr[5];
            const std::uint32_t out = ctx.gpr[6];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] query-offset mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " buf=" << psprecomp::hex32(buffer)
                          << " out=" << psprecomp::hex32(out) << "\n";
            }
            if (buffer == 0u || out == 0u ||
                !rt.memory().contains(buffer, 16u) ||
                !rt.memory().contains(out, 4u)) {
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            if (rt.memory().load32(buffer) != 0x464D5350u) {
                rt.memory().store32(out, 0u);
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            const std::uint32_t offset = byte_swap32(rt.memory().load32(buffer + 8u));
            rt.memory().store32(out, offset);
            if (MpegState *state = mpeg_state_for(rt, ctx.gpr[4]); state != nullptr)
                state->stream_offset = offset;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x611E9E11u, // sceMpegQueryStreamSize
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t buffer = ctx.gpr[4];
            const std::uint32_t out = ctx.gpr[5];
            if (buffer == 0u || out == 0u ||
                !rt.memory().contains(buffer, 16u) ||
                !rt.memory().contains(out, 4u)) {
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            if (rt.memory().load32(buffer) != 0x464D5350u) {
                rt.memory().store32(out, 0u);
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            const std::uint32_t size = byte_swap32(rt.memory().load32(buffer + 0x0Cu));
            rt.memory().store32(out, size);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xB5F6DC87u, // sceMpegRingbufferAvailableSize
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ringbuffer = ctx.gpr[4];
            if (ringbuffer == 0u || !rt.memory().contains(ringbuffer, 48u)) {
                ctx.set_gpr(2, 0x80020005u);
                return;
            }
            const std::uint32_t packets = rt.memory().load32(ringbuffer + 0u);
            const std::uint32_t used = rt.memory().load32(ringbuffer + 12u);
            ctx.set_gpr(2, packets > used ? packets - used : 0u);
        });
    runtime.register_hle("sceMpeg", 0x42560F23u, // sceMpegRegistStream
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::uint32_t next_stream = 1u;
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] regist mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " type=" << ctx.gpr[5] << " num=" << ctx.gpr[6] << "\n";
            }
            ctx.set_gpr(2, next_stream++);
        });
    runtime.register_hle("sceMpeg", 0x591A4AA2u, // sceMpegUnRegistStream
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xA780CF7Eu, // sceMpegMallocAvcEsBuf
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("sceMpeg", 0xCEB870B1u, // sceMpegFreeAvcEsBuf
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x167AFD9Eu, // sceMpegInitAu
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t buffer = ctx.gpr[5];
            const std::uint32_t au = ctx.gpr[6];
            if (au == 0u || !rt.memory().contains(au, 0x18u)) {
                ctx.set_gpr(2, 0x80628002u);
                return;
            }
            const bool avc = buffer == 1u || buffer == 2u;
            rt.memory().store32(au + 0x00u, 0u); // pts low
            rt.memory().store32(au + 0x04u, 0u); // pts high
            rt.memory().store32(au + 0x08u, avc ? 0u : 0xFFFFFFFFu); // dts low
            rt.memory().store32(au + 0x0Cu, avc ? 0u : 0xFFFFFFFFu); // dts high
            rt.memory().store32(au + 0x10u, 0u); // esBuffer
            rt.memory().store32(au + 0x14u, avc ? 2048u : 2112u); // esSize
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xB240A59Eu, // sceMpegRingbufferPut
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ringbuffer = ctx.gpr[4];
            if (ringbuffer == 0u || !rt.memory().contains(ringbuffer, 48u)) {
                ctx.set_gpr(2, 0x80020005u);
                return;
            }
            // Decoding is driven directly from the host stream; the guest
            // ringbuffer still needs plausible packet counts for its loop.
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] ringbuffer-put ring=" << psprecomp::hex32(ringbuffer)
                          << " num=" << ctx.gpr[5] << " avail=" << ctx.gpr[6] << "\n";
            }
            const std::uint32_t packets = rt.memory().load32(ringbuffer + 0u);
            rt.memory().store32(ringbuffer + 8u, packets);
            rt.memory().store32(ringbuffer + 12u, packets);
            ctx.set_gpr(2, ctx.gpr[5]);
        });
    runtime.register_hle("sceMpeg", 0x800C44DFu, // sceMpegAtracDecode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t init = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] atrac-decode mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " au=" << psprecomp::hex32(ctx.gpr[5])
                          << " buffer=" << psprecomp::hex32(ctx.gpr[6])
                          << " init=" << psprecomp::hex32(init) << "\n";
            }
            const std::uint32_t audio_buffer = ctx.gpr[6];
            std::size_t decoded_bytes = 0u;
            if (audio_buffer != 0u && rt.memory().contains(audio_buffer, 8192u)) {
                std::string error;
                if (ensure_atrac_player(rt, error)) {
                    std::vector<std::uint8_t> pcm(8192u);
                    decoded_bytes = g_atrac_player->read_pcm(pcm.data(), pcm.size());
                    if (mpeg_diag()) {
                        static std::uint32_t reads = 0u;
                        if (reads < 8u) {
                            ++reads;
                            std::cerr << "[tow1-mpeg] atrac-pcm bytes="
                                      << decoded_bytes << " frames="
                                      << g_atrac_player->decoded_frames() << "\n";
                        }
                    }
                    rt.memory().copy_in(audio_buffer,
                        std::span<const std::uint8_t>(pcm.data(), pcm.size()));
                } else {
                    if (mpeg_diag()) std::cerr << "[tow1-mpeg] " << error << "\n";
                    rt.memory().zero(audio_buffer, 8192u);
                }
            }
            if (init != 0u && rt.memory().contains(init, 4u))
                rt.memory().store32(init, decoded_bytes != 0u ? 1u : 0u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xFE246728u, // sceMpegGetAvcAu
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t au = ctx.gpr[6];
            const std::uint32_t attr = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] get-avc mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " stream=" << psprecomp::hex32(ctx.gpr[5])
                          << " au=" << psprecomp::hex32(au) << " attr=" << psprecomp::hex32(attr) << "\n";
            }
            if (au != 0u && rt.memory().contains(au, 0x18u)) {
                rt.memory().zero(au, 0x18u);
                rt.memory().store32(au + 0x14u, 2048u);
            }
            if (attr != 0u && rt.memory().contains(attr, 4u))
                rt.memory().store32(attr, 0u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xE1CE83A7u, // sceMpegGetAtracAu
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t au = ctx.gpr[6];
            const std::uint32_t attr = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] get-atrac mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " stream=" << psprecomp::hex32(ctx.gpr[5])
                          << " au=" << psprecomp::hex32(au) << "\n";
            }
            if (au != 0u && rt.memory().contains(au, 0x18u)) {
                rt.memory().zero(au, 0x18u);
                rt.memory().store32(au + 0x14u, 0u);
            }
            if (attr != 0u && rt.memory().contains(attr, 4u))
                rt.memory().store32(attr, 0u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x0E3C2E9Du, // sceMpegAvcDecode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            ++g_avc_decode_calls;
            if (std::getenv("PSPRECOMP_TOW1_MPEG_UNPACED") == nullptr) {
                using video_clock = std::chrono::steady_clock;
                static video_clock::time_point next_video = video_clock::now();
                const video_clock::time_point now = video_clock::now();
                if (now < next_video) {
                    if (ctx.gpr[8] != 0u && rt.memory().contains(ctx.gpr[8], 4u))
                        rt.memory().store32(ctx.gpr[8], 1u);
                    ctx.set_gpr(2, 0u);
                    return;
                }
                next_video = now + std::chrono::microseconds(33366);
            }
            const std::uint32_t init = ctx.gpr[8];
            const std::uint32_t buffer_ptr = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow1-mpeg] avc-decode mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " au=" << psprecomp::hex32(ctx.gpr[5])
                          << " width=" << ctx.gpr[6]
                          << " buffer_ptr=" << psprecomp::hex32(buffer_ptr)
                          << " init=" << psprecomp::hex32(init)
                          << " dest=" << (buffer_ptr != 0u && rt.memory().contains(buffer_ptr, 4u)
                                               ? psprecomp::hex32(rt.memory().load32(buffer_ptr)) : "?")
                          << "\n";
            }
            std::string error;
            if (!ensure_mpeg_player(rt, error)) {
                if (mpeg_diag()) std::cerr << "[tow1-mpeg] " << error << "\n";
                if (init != 0u && rt.memory().contains(init, 4u))
                    rt.memory().store32(init, 0u);
                ctx.set_gpr(2, 1u);
                return;
            }
            const bool skip_by_env =
                std::getenv("PSPRECOMP_TOW1_SKIP_MOVIE") != nullptr &&
                g_scheduler.now_us >= 5000000u;
            if (!g_mpeg_video_eof &&
                (window_buttons() != 0u || skip_by_env) &&
                g_mpeg_player->decoded_frames() > 0u) {
                if (mpeg_diag()) std::cerr << "[tow1-mpeg] skip requested by pad\n";
                g_mpeg_player->skip_to_end();
            }
            int decoded_width = 0;
            int decoded_height = 0;
            const bool decoded = g_mpeg_player->decode_next(
                g_mpeg_frame, decoded_width, decoded_height);
            if (!decoded) {
                g_mpeg_video_eof = true;
                if (mpeg_diag()) std::cerr << "[tow1-mpeg] end of video stream\n";
                if (init != 0u && rt.memory().contains(init, 4u))
                    rt.memory().store32(init, 0u);
                ctx.set_gpr(2, 0u);
                return;
            }
            const std::uint32_t destination =
                buffer_ptr != 0u && rt.memory().contains(buffer_ptr, 4u)
                    ? rt.memory().load32(buffer_ptr) : 0u;
            MpegState *state = mpeg_state_for(rt, ctx.gpr[4]);
            const std::uint32_t width = ctx.gpr[6] != 0u ? ctx.gpr[6]
                : (state != nullptr && state->frame_width != 0u ? state->frame_width : 480u);
            const std::uint32_t pixel_mode = state != nullptr ? state->pixel_mode : 3u;
            const bool wrote = write_mpeg_frame(
                rt, destination, width, pixel_mode, g_mpeg_frame,
                decoded_width, decoded_height);
            if (!wrote) {
                if (mpeg_diag()) {
                    std::cerr << "[tow1-mpeg] write failed dest="
                              << psprecomp::hex32(destination) << " width=" << width
                              << " pixel=" << pixel_mode << "\n";
                }
                if (init != 0u && rt.memory().contains(init, 4u))
                    rt.memory().store32(init, 0u);
                ctx.set_gpr(2, 1u);
                return;
            }
            if (state != nullptr) ++state->video_frames;
            if (init != 0u && rt.memory().contains(init, 4u))
                rt.memory().store32(init, 1u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x0F6C18D7u, // sceMpegAvcDecodeDetail
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t detail = ctx.gpr[5];
            if (detail != 0u && rt.memory().contains(detail, 36u)) {
                rt.memory().zero(detail, 36u);
                MpegState *state = mpeg_state_for(rt, ctx.gpr[4]);
                rt.memory().store32(detail + 0u, g_mpeg_video_eof ? 0u : 1u);
                rt.memory().store32(detail + 4u, state != nullptr ? state->video_frames : 0u);
                rt.memory().store32(detail + 8u, 480u);
                rt.memory().store32(detail + 12u, 272u);
                rt.memory().store32(detail + 32u, g_mpeg_video_eof ? 0u : 1u);
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x740FCCD1u, // sceMpegAvcDecodeStop
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t status = ctx.gpr[7];
            if (status != 0u && rt.memory().contains(status, 4u))
                rt.memory().store32(status, 0u);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xBE45C5A6u, // sceMpegAvcDecodeMode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (mpeg_diag() && ctx.gpr[5] != 0u && rt.memory().contains(ctx.gpr[5], 8u)) {
                std::cerr << "[tow1-mpeg] decode-mode mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " mode=" << rt.memory().load32(ctx.gpr[5])
                          << " pixel=" << rt.memory().load32(ctx.gpr[5] + 4u) << "\n";
            }
            if (MpegState *state = mpeg_state_for(rt, ctx.gpr[4]); state != nullptr &&
                ctx.gpr[5] != 0u && rt.memory().contains(ctx.gpr[5], 8u)) {
                state->pixel_mode = rt.memory().load32(ctx.gpr[5] + 4u);
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x4571CC64u, // sceMpegAvcDecodeFlush
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xF8DCB679u, // sceMpegQueryAtracEsSize
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t size_out = ctx.gpr[5];
            const std::uint32_t output_out = ctx.gpr[6];
            if (size_out != 0u && rt.memory().contains(size_out, 4u))
                rt.memory().store32(size_out, 2112u);
            if (output_out != 0u && rt.memory().contains(output_out, 4u))
                rt.memory().store32(output_out, 8192u);
            ctx.set_gpr(2, 0u);
        });

    // Minimal GE/display contract for boot. Lists are accepted and reported
    // complete; no command-stream rasterization happens yet.
    runtime.register_hle("sceGe_user", 0xE47E40E4u, // sceGeEdramGetAddr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0x04000000u);
        });
    runtime.register_hle("sceGe_user", 0x1F6752ADu, // sceGeEdramGetSize
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0x00200000u);
        });
    runtime.register_hle("sceGe_user", 0xAB49E76Au, // sceGeListEnQueue
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            dump_ge_list(rt, ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], "enqueue");
            ge_enqueue(rt, ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]);
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("sceGe_user", 0x1C0D95A6u, // sceGeListEnQueueHead
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            dump_ge_list(rt, ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], "enqueue-head");
            ge_enqueue(rt, ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]);
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("sceGe_user", 0x03444EB4u, // sceGeListSync
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0xB287BD61u, // sceGeDrawSync
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0xE0D68148u, // sceGeListUpdateStallAddr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0xA4FC06A4u, // sceGeSetCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0x05DB22CEu, // sceGeUnsetCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0xB448EC0Du, // sceGeBreak
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceGe_user", 0x4C06E472u, // sceGeContinue
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("sceDisplay", 0x0E20F177u, // sceDisplaySetMode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_display_mode = ctx.gpr[4];
            g_display_width = ctx.gpr[5];
            g_display_height = ctx.gpr[6];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x289D82FEu, // sceDisplaySetFrameBuf
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            g_framebuffer = ctx.gpr[4];
            g_framebuffer_stride = ctx.gpr[5];
            g_framebuffer_format = ctx.gpr[6];
            if (ge_diag()) {
                std::cerr << "[tow1-ge] framebuf=" << psprecomp::hex32(g_framebuffer)
                          << " stride=" << g_framebuffer_stride
                          << " format=" << g_framebuffer_format
                          << " sync=" << psprecomp::hex32(ctx.gpr[7]) << "\n";
            }
            ge_set_framebuffer(rt, g_framebuffer, g_framebuffer_stride, g_framebuffer_format);
            if (std::getenv("PSPRECOMP_TOW1_VBLANK_DIAG") != nullptr) {
                static std::uint64_t frames = 0u;
                static std::uint64_t last_avc = 0u;
                if (frames < 80u || (frames % 1000u) == 0u)
                    std::cerr << "[tow1-vblank] frame=" << frames
                              << " now_us=" << g_scheduler.now_us
                              << " vcount=" << (g_scheduler.now_us / 16683u)
                              << " avc_delta=" << (g_avc_decode_calls - last_avc) << "\n";
                last_avc = g_avc_decode_calls;
                ++frames;
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x36CDFADEu, // sceDisplayWaitVblankStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            display_wait_tick();
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x46F186C3u, // sceDisplayWaitVblankStartCB
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            display_wait_tick();
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x8EB9EC49u, // sceDisplayWaitVblankCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            display_wait_tick();
            if (std::getenv("PSPRECOMP_TOW1_WAIT_DIAG") != nullptr) {
                static std::uint32_t logged = 0u;
                if (logged < 12u) {
                    ++logged;
                    std::cerr << "[tow1-wait] pc=" << psprecomp::hex32(ctx.pc)
                              << " ra=" << psprecomp::hex32(ctx.gpr[31])
                              << " flag=" << psprecomp::hex32(
                                     rt.memory().load32(0x08DB61D0u))
                              << " state=" << psprecomp::hex32(
                                     rt.memory().load32(0x08E0E3C8u))
                              << " next=" << psprecomp::hex32(
                                     rt.memory().load32(0x08E0E3CCu))
                              << " now=" << g_scheduler.now_us << "\n";
                }
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x984C27E7u, // sceDisplayWaitVblank
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            display_wait_tick();
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x9C6EAAD7u, // sceDisplayGetVcount
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const char *tick_text = std::getenv("PSPRECOMP_TOW1_VCOUNT_TICK");
            if (tick_text == nullptr || *tick_text == '\0')
                tick_text = "1000";
            if (tick_text != nullptr) {
                static const std::uint64_t interval =
                    std::max<std::uint64_t>(1u, std::strtoull(tick_text, nullptr, 0));
                static std::uint64_t calls = 0u;
                if (++calls >= interval) {
                    calls = 0u;
                    if (g_vblank_wait_generation != g_vblank_tick_generation) {
                        // A real vblank wait already produced this frame's time.
                        g_vblank_tick_generation = g_vblank_wait_generation;
                    } else {
                        g_scheduler.now_us += 16683u;
                        g_next_vblank_us = g_scheduler.now_us + 16683u;
                        signal_sleep_semaphore_for_vblank();
                    }
                }
            }
            if (std::getenv("PSPRECOMP_TOW1_WAIT_DIAG") != nullptr) {
                static std::uint32_t logged = 0u;
                if (logged < 16u) {
                    ++logged;
                    std::cerr << "[tow1-vcount] ra=" << psprecomp::hex32(ctx.gpr[31])
                              << " pc=" << psprecomp::hex32(ctx.pc)
                              << " now=" << g_scheduler.now_us << "\n";
                }
            }
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_scheduler.now_us / 16683u));
        });
    runtime.register_hle("sceDisplay", 0xDBA6C4C4u, // sceDisplayGetFramePerSec
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.fpr[0] = 59.94f;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0xEEDA2E54u, // sceDisplayGetFrameBuf
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[4] != 0u && rt.memory().contains(ctx.gpr[4], 4u))
                rt.memory().store32(ctx.gpr[4], g_framebuffer);
            if (ctx.gpr[5] != 0u && rt.memory().contains(ctx.gpr[5], 4u))
                rt.memory().store32(ctx.gpr[5], g_framebuffer_stride);
            if (ctx.gpr[6] != 0u && rt.memory().contains(ctx.gpr[6], 4u))
                rt.memory().store32(ctx.gpr[6], g_framebuffer_format);
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("scePower", 0x04B7766Eu, // scePowerRegisterCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("scePower", 0xDFA8BAF8u, // scePowerUnregisterCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("sceSuspendForUser", 0xEADB1BD7u, // sceKernelPowerLock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, ctx.gpr[4] == 0u ? 0u : 0x80000107u);
        });
    runtime.register_hle("sceSuspendForUser", 0x3AEE7261u, // sceKernelPowerUnlock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, ctx.gpr[4] == 0u ? 0u : 0x80000107u);
        });
    runtime.register_hle("sceSuspendForUser", 0x090CCB3Fu, // sceKernelPowerTick
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceSuspendForUser", 0x3E0271D3u, // sceKernelVolatileMemLock
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            constexpr std::uint32_t volatile_base = 0x08400000u;
            constexpr std::uint32_t volatile_size = 0x00400000u;
            if (ctx.gpr[4] != 0u) {
                ctx.set_gpr(2, 0x80000107u);
                return;
            }
            if (g_volatile_memory_locked) {
                ctx.set_gpr(2, 0x80000021u);
                return;
            }
            if (!rt.memory().contains(volatile_base, volatile_size) ||
                !rt.memory().contains(ctx.gpr[5], 4u) ||
                !rt.memory().contains(ctx.gpr[6], 4u)) {
                ctx.set_gpr(2, 0x800200D3u);
                return;
            }
            rt.memory().store32(ctx.gpr[5], volatile_base);
            rt.memory().store32(ctx.gpr[6], volatile_size);
            rt.memory().zero(volatile_base, volatile_size);
            g_volatile_memory_locked = true;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceSuspendForUser", 0xA569E425u, // sceKernelVolatileMemUnlock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[4] != 0u) {
                ctx.set_gpr(2, 0x80000107u);
                return;
            }
            if (!g_volatile_memory_locked) {
                ctx.set_gpr(2, 0x800201AEu);
                return;
            }
            g_volatile_memory_locked = false;
            ctx.set_gpr(2, 0u);
        });

    // The disc is modelled as permanently present/ready/readable. ToW1 only
    // imports the activate/check/stat/wait/register subset.
    runtime.register_hle("sceUmdUser", 0xAEE7404Du, // sceUmdRegisterUMDCallBack
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUmdUser", 0x46EBB729u, // sceUmdCheckMedium
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("sceUmdUser", 0x6B4A146Cu, // sceUmdGetDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0x32u); // PRESENT | READY | READABLE
        });
    runtime.register_hle("sceUmdUser", 0x8EF08FCEu, // sceUmdWaitDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUmdUser", 0x4A9E5E29u, // sceUmdWaitDriveStatCB
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUmdUser", 0x6AF9B50Au, // sceUmdWaitDriveStatWithTimer
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUmdUser", 0xC6183D47u, // sceUmdActivate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("sceLibFont", 0x67F17ED7u, // sceFontNewLib
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_new_lib(rt, ctx); });
    runtime.register_hle("sceLibFont", 0xA834319Du, // sceFontOpen
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_open(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x099EF33Cu, // sceFontFindOptimumFont
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_find_optimum(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x0DA7535Eu, // sceFontGetFontInfo
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_get_info(rt, ctx); });
    runtime.register_hle("sceLibFont", 0xDCC80C2Fu, // sceFontGetCharInfo
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_get_char_info(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x980F4895u, // sceFontGetCharGlyphImage
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_get_glyph(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x5C3E4A9Eu, // sceFontGetCharImageRect
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_get_char_image_rect(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x57FCB733u, // sceFontOpenUserFile
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_open_user(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0xBB8E7FE6u, // sceFontOpenUserMemory
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_open_user(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x27F6E642u, // sceFontGetNumFontList
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_get_num_list(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0xBC75D85Bu, // sceFontGetFontList
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_get_font_list(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x5333322Du, // sceFontGetFontInfoByIndexNumber
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_get_info_by_index(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x681E61A7u, // sceFontFindFont
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_find_font(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x2F67356Au, // sceFontCalcMemorySize
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_calc_memory_size(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0xAA3DE7B5u, // sceFontGetShadowInfo
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_shadow_info(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x48B06520u, // sceFontGetShadowImageRect
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_shadow_rect(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x568BE516u, // sceFontGetShadowGlyphImage
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_shadow_glyph(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0xCA1E6945u, // sceFontGetCharGlyphImage_Clip
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_glyph_clip(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x5DCF6858u, // sceFontGetShadowGlyphImage_Clip
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_glyph_clip(rt, ctx);
        });
    runtime.register_hle("sceLibFont", 0x48293280u, // sceFontSetResolution
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceLibFont", 0x74B21701u, // sceFontPixelToPointH
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.fpr[0] = ctx.fpr[12];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceLibFont", 0xF8F0752Eu, // sceFontPixelToPointV
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.fpr[0] = ctx.fpr[12];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceLibFont", 0x472694CDu, // sceFontPointToPixelH
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.fpr[0] = ctx.fpr[12];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceLibFont", 0x3C4B7E82u, // sceFontPointToPixelV
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.fpr[0] = ctx.fpr[12];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceLibFont", 0x3AEA8CB6u, // sceFontClose
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_close(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x574B6FBCu, // sceFontDoneLib
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_done(rt, ctx); });
    runtime.register_hle("sceLibFont", 0xEE232411u, // sceFontSetAltCharacterCode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_set_alt(rt, ctx); });

    runtime.register_hle("LoadExecForUser", 0x4AC57943u, // sceKernelRegisterExitCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("LoadExecForUser", 0x05572A5Fu, // sceKernelExitGame
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (std::getenv("PSPRECOMP_TOW1_EXIT_DIAG") != nullptr) {
                std::cerr << "[tow1-exit] ExitGame uid=" << g_scheduler.current_uid
                          << " pc=" << psprecomp::hex32(ctx.pc)
                          << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
            }
            rt.stop("sceKernelExitGame");
        });

    runtime.register_hle("sceImpose", 0x36AA6E91u, // sceImposeSetLanguageMode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("sceUtility", 0x2A2B3DE0u, // sceUtilityLoadModule
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceUtility", 0xE49BFE92u, // sceUtilityUnloadModule
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("sceUtility", 0xA5DA2406u, // sceUtilityGetSystemParamInt
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            get_system_param_int(rt, ctx);
        });
    runtime.register_hle("sceUtility", 0x34B78343u, // sceUtilityGetSystemParamString
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            get_system_param_string(rt, ctx);
        });
    // Savedata dialog: complete immediately. The game still polls the dialog
    // state machine, so keep a tiny status record rather than returning a
    // missing-import stop.
    runtime.register_hle("sceUtility", 0x50C4CD57u, // sceUtilitySavedataInitStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // This port has no interactive save dialog.  The title's loader
            // treats SHUTDOWN (3) as the completed-dialog state and then
            // proceeds to module loading; NONE (0) makes it bail out early.
            g_savedata_status = 3;
            if (std::getenv("PSPRECOMP_TOW1_SAVEDATA_DIAG") != nullptr) {
                std::cerr << "[tow1-save] init param=" << psprecomp::hex32(ctx.gpr[4])
                          << " -> status=3\n";
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUtility", 0x8874DBE0u, // sceUtilitySavedataGetStatus
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (std::getenv("PSPRECOMP_TOW1_SAVEDATA_DIAG") != nullptr) {
                std::cerr << "[tow1-save] get -> " << g_savedata_status << "\n";
            }
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_savedata_status));
        });
    runtime.register_hle("sceUtility", 0xD4B95FFBu, // sceUtilitySavedataUpdate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // One update is enough to finish the non-interactive dialog.
            if (std::getenv("PSPRECOMP_TOW1_SAVEDATA_DIAG") != nullptr) {
                std::cerr << "[tow1-save] update " << ctx.gpr[4]
                          << " -> status=0\n";
            }
            g_savedata_status = 0;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUtility", 0x9790B33Cu, // sceUtilitySavedataShutdownStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_savedata_status = 0;
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("Kernel_Library", 0x092968F4u, // sceKernelCpuSuspendIntr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("Kernel_Library", 0x5F10D406u, // sceKernelCpuResumeIntr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("ThreadManForUser", 0xE1619D7Cu, // sceKernelSysClock2USecWide
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint64_t clock =
                static_cast<std::uint64_t>(ctx.gpr[4]) |
                (static_cast<std::uint64_t>(ctx.gpr[5]) << 32u);
            const std::uint32_t usec_pointer = ctx.gpr[6];
            const std::uint32_t sec_pointer = ctx.gpr[7];
            if (usec_pointer != 0u && rt.memory().contains(usec_pointer, 4u)) {
                rt.memory().store32(usec_pointer,
                                    static_cast<std::uint32_t>(clock % 1000000u));
            }
            if (sec_pointer != 0u && rt.memory().contains(sec_pointer, 4u)) {
                rt.memory().store32(sec_pointer,
                                    static_cast<std::uint32_t>(clock / 1000000u));
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("ThreadManForUser", 0x110DEC9Au, // sceKernelUSec2SysClock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, ctx.gpr[4]);
            ctx.set_gpr(3, 0u);
        });

    runtime.register_hle("ThreadManForUser", 0x369ED59Du, // sceKernelGetSystemTimeLow
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_scheduler.now_us));
        });
    runtime.register_hle("ThreadManForUser", 0x82BC5777u, // sceKernelGetSystemTimeWide
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_scheduler.now_us));
            ctx.set_gpr(3, static_cast<std::uint32_t>(g_scheduler.now_us >> 32u));
        });

    runtime.register_hle("ThreadManForUser", 0x9ACE131Eu, // sceKernelSleepThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            sleep_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xD59EAD2Fu, // sceKernelWakeupThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wakeup_thread(rt, ctx);
        });

    runtime.register_hle("ThreadManForUser", 0x8125221Du, // sceKernelCreateMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_mbx(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xE9B3061Eu, // sceKernelSendMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            send_mbx(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x18260574u, // sceKernelReceiveMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            receive_mbx(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x0D81716Au, // sceKernelPollMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            poll_mbx(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x86255ADAu, // sceKernelDeleteMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delete_mbx(rt, ctx);
        });

    runtime.register_hle("ThreadManForUser", 0x55C20A00u, // sceKernelCreateEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_event_flag(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x1FB15A32u, // sceKernelSetEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            set_event_flag(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x812346E4u, // sceKernelClearEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            clear_event_flag(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x402FCF22u, // sceKernelWaitEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wait_event_flag(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xEF9E4C70u, // sceKernelDeleteEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delete_event_flag(rt, ctx);
        });

    runtime.register_hle("ThreadManForUser", 0xB7D098C6u, // sceKernelCreateMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_mutex(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xB011B11Fu, // sceKernelLockMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            lock_mutex(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x6B30100Fu, // sceKernelUnlockMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            unlock_mutex(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xF8170FBEu, // sceKernelDeleteMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delete_mutex(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xAA73C935u, // sceKernelExitThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (auto current = g_scheduler.threads.find(g_scheduler.current_uid);
                current != g_scheduler.threads.end()) {
                current->second.exit_status = ctx.gpr[4];
            }
            complete_current_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x840E8133u, // sceKernelWaitThreadEndCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wait_thread_end(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x6D212BACu, // sceKernelWaitSemaCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wait_semaphore(rt, ctx);
        });

    runtime.register_hle("ThreadManForUser", 0xEA748E31u, // sceKernelChangeCurrentThreadAttr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t attributes = ctx.gpr[4];
            const std::uint32_t mask = ctx.gpr[5];
            const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
            if (current != g_scheduler.threads.end()) {
                current->second.attributes =
                    (current->second.attributes & ~mask) | (attributes & mask);
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("ThreadManForUser", 0x71BC9871u, // sceKernelChangeThreadPriority
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
            if (uid == 0) uid = g_scheduler.current_uid;
            const auto thread = g_scheduler.threads.find(uid);
            if (thread == g_scheduler.threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            thread->second.priority = ctx.gpr[5];
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("ThreadManForUser", 0xE81CAF8Fu, // sceKernelCreateCallback
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_callback(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xEDBA5844u, // sceKernelDeleteCallback
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delete_callback(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x2A3D44FFu, // sceKernelGetCallbackCount
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("ThreadManForUser", 0x349D6D6Cu, // sceKernelCheckCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // No notification path is imported by ToW1 at this stage; a null
            // callback object has nothing to dispatch.
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("ThreadManForUser", 0xD6DA4BA1u, // sceKernelCreateSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_semaphore(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x4E3A1105u, // sceKernelWaitSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wait_semaphore(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x3F53E640u, // sceKernelSignalSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            signal_semaphore(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x58B1F937u, // sceKernelPollSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            poll_semaphore(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x28B6489Cu, // sceKernelDeleteSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delete_semaphore(rt, ctx);
        });

    runtime.register_hle("ModuleMgrForUser", 0x977DE386u, // sceKernelLoadModule
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { load_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0x50F0C1ECu, // sceKernelStartModule
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { start_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0xD1FF982Au, // sceKernelStopModule
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { stop_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0x2E0911AAu, // sceKernelUnloadModule
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { unload_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0x8F2DF740u, // sceKernelStopUnloadSelfModuleWithStatus
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            stop_unload_self_module_with_status(rt, ctx);
        });

    runtime.register_hle("ModuleMgrForUser", 0xF0A26395u, // sceKernelGetModuleId
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, kMainModuleUid);
        });
    runtime.register_hle("ModuleMgrForUser", 0xD8B73127u, // sceKernelGetModuleIdByAddress
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            const bool in_main_module =
                address >= psprecomp::kDefaultPspUserLoadBase && address < g_module_end;
            ctx.set_gpr(2, in_main_module ? kMainModuleUid : 0x80020130u);
        });

    runtime.register_hle("ThreadManForUser", 0x446D8DE6u, // sceKernelCreateThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            create_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xF475845Du, // sceKernelStartThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            start_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x809CE29Bu, // sceKernelExitDeleteThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (auto current = g_scheduler.threads.find(g_scheduler.current_uid);
                current != g_scheduler.threads.end()) {
                current->second.exit_status = ctx.gpr[4];
            }
            complete_current_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x9FA03CD3u, // sceKernelDeleteThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (std::getenv("PSPRECOMP_TOW1_EXIT_DIAG") != nullptr) {
                std::cerr << "[tow1-exit] DeleteThread uid=" << g_scheduler.current_uid
                          << " pc=" << psprecomp::hex32(ctx.pc)
                          << " ra=" << psprecomp::hex32(ctx.gpr[31])
                          << " a0=" << psprecomp::hex32(ctx.gpr[4])
                          << " now=" << g_scheduler.now_us << "\n";
            }
            delete_thread(rt, ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x293B45B8u, // sceKernelGetThreadId
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_scheduler.current_uid));
        });
    runtime.register_hle("ThreadManForUser", 0xCEADEB47u, // sceKernelDelayThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delay_current_thread(rt, ctx, ctx.gpr[4]);
        });
    runtime.register_hle("ThreadManForUser", 0x68DA9E36u, // sceKernelDelayThreadCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            delay_current_thread(rt, ctx, ctx.gpr[4]);
        });
    runtime.register_hle("ThreadManForUser", 0x278C0DF5u, // sceKernelWaitThreadEnd
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wait_thread_end(rt, ctx);
        });

    const std::uint64_t interval = timeslice_interval();
    if (interval != 0u) {
        psprecomp::set_runtime_starvation_hook(&starvation_preempt, interval);
    }

    std::cout << "tow-1 HLE: scheduler and kernel objects initialized"
              << " (timeslice=" << interval << ")\n";
}

} // namespace tow1

