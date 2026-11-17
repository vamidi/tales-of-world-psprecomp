#include "tow2_hle.h"

#include "tow2_atrac.hpp"
#include "tow2_ezbind.hpp"
#include "tow2_mpeg.hpp"
#include "tow2_window.hpp"

#include "generated_units.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psprecomp {
// Defined by the renamed libfile.prx corpus.
void libfile_register_generated_functions(Runtime &runtime);
}

namespace tow2 {
namespace {

constexpr std::uint32_t kUserRamTop = 0x0A000000u;
constexpr std::uint32_t kModuleStackSize = 0x10000u;
constexpr std::uint32_t kThreadReturnPc = 0x00000000u;
constexpr std::uint32_t kTitleCreatorThread = 0x0F000000u;
constexpr std::uint32_t kTitleStateThread = 0x0F000001u;
constexpr std::uint32_t kTitleThreadCreator = 0x0F000002u;
constexpr std::uint32_t kTitleState2Thread = 0x0F000003u;

// Secondary plain-ELF module: libfile.prx is loaded and executed at a free
// kernel-area base instead of being HLE-stubbed.
constexpr std::uint32_t kLibfileBase = 0x08000000u;
constexpr std::uint32_t kLibfileStackTop = 0x08040000u;
constexpr std::uint32_t kLibfileStackSize = 0x00020000u;
std::uint32_t g_libfile_gp = 0;
std::uint32_t g_libfile_stack_top = 0;
bool g_libfile_loaded = false;

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

// The title's main loop waits on a semaphore named "SleepSema". The scheduler
// tick models a vblank signal for it; opt out with
// PSPRECOMP_TOW2_NO_VBLANK_SEMA=1.
std::int32_t g_sleep_semaphore_uid = -1;
std::uint64_t g_next_vblank_us = 16683u;
// Set by the title mount-thread diagnostic so the sleep-semaphore handshake
// does not wake user_main before the mount has completed.
std::int32_t g_mount_thread_uid = -1;

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

// EZBIND archives opened by the guest, parsed natively so the host can resolve
// entries without re-running the guest container parser.
std::unordered_map<std::string, tow2::EzbindArchive> g_ezbind_archives;

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
std::int32_t g_next_module_uid = 0x700;

std::uint32_t g_module_end = psprecomp::kDefaultPspUserLoadBase;

// Boot-level display state. The real GE interpreter/presenter arrives in a
// later phase; these values let the guest configure a framebuffer now.
std::uint32_t g_display_mode = 0u;
std::uint32_t g_display_width = 480u;
std::uint32_t g_display_height = 272u;
std::uint32_t g_framebuffer = 0u;
std::uint32_t g_framebuffer_stride = 0u;
std::uint32_t g_framebuffer_format = 0u;
bool g_volatile_memory_locked = false;
std::int32_t g_savedata_status = 0;

std::array<bool, 8> g_audio_channel_reserved{};
std::array<std::uint32_t, 8> g_audio_channel_samples{};
struct SasState {
    std::uint32_t core{};
    std::int32_t grain{};
    std::int32_t max_voices{};
    std::int32_t output_mode{};
    std::int32_t sample_rate{44100};
    std::uint32_t end_flag{0};
};
SasState g_sas;

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
std::unique_ptr<tow2::MpegPlayer> g_mpeg_player;
std::unique_ptr<tow2::AtracPlayer> g_atrac_player;
std::vector<std::uint8_t> g_mpeg_frame;
bool g_mpeg_video_eof = false;
std::string g_pmf_host_path;
std::uint32_t g_pmf_packet_count = 0u;
std::uint32_t g_movie_fp = 0u;
std::uint32_t g_movie_vcount = 0u;
std::uint32_t g_movie_counter_18 = 0u;
std::uint32_t g_movie_counter_1c = 0u;
std::uint32_t g_movie_counter_30 = 0u;
std::uint32_t g_movie_counter_34 = 0u;
std::uint32_t g_title_object = 0u;
bool g_movie_done = false;
psprecomp::GuestMemory *g_mpeg_memory = nullptr;

bool thread_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_THREAD_DIAG") != nullptr;
    return enabled;
}

bool partition_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_PARTITION_DIAG") != nullptr;
    return enabled;
}

bool file_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_FILE_DIAG") != nullptr;
    return enabled;
}

bool module_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_MODULE_DIAG") != nullptr;
    return enabled;
}

bool mpeg_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_TOW2_MPEG_DIAG") != nullptr;
    return enabled;
}

bool sound_diag() {
    static const bool enabled = std::getenv("PSPRECOMP_TOW2_SOUND_DIAG") != nullptr;
    return enabled;
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
        std::cerr << "[tow2] switch reason=" << reason << " uid=" << uid
                  << " name=" << thread->second.name
                  << " pc=" << psprecomp::hex32(ctx.pc)
                  << " sp=" << psprecomp::hex32(ctx.gpr[29])
                  << " ready=" << g_scheduler.ready.size() << "\n";
    }
    return true;
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

void title_creator_thread_entry(psprecomp::Runtime &runtime,
                                psprecomp::AllegrexContext &ctx) {
    const std::uint32_t scene =
        runtime.memory().contains(0x08D14F2Cu, 4u)
            ? runtime.memory().load32(0x08D14F2Cu) : g_title_object;
    ctx.gpr[4] = scene;
    ctx.gpr[5] = 0u;
    ctx.gpr[6] = 0u;
    ctx.gpr[7] = 0u;
    ctx.gpr[31] = kThreadReturnPc;
    ctx.pc = 0x0896FEC4u;
}

std::uint32_t g_scene_watch_address = 0u;
std::uint32_t g_scene_watch_value = 0xFFFFFFFFu;
// Actual `this` passed to the scene registration method 0x0881C224.
std::uint32_t g_asset_owner = 0u;
// One-shot: after the title frame entry has run, locate the registered asset
// container (vtable 0x08C44B08) and invoke one of its virtual methods.
int g_drive_container_countdown = -1;

constexpr std::uint32_t kIsolatedReturnSentinel = 0xDEADBEEFu;
bool run_isolated_aot(psprecomp::Runtime &runtime,
                      psprecomp::AllegrexContext &ctx, std::uint32_t entry,
                      std::uint32_t return_sentinel, int max_steps,
                      const char *tag);

void scene_caller_hook(psprecomp::Runtime &runtime,
                       psprecomp::AllegrexContext &ctx,
                       std::uint32_t dispatch_pc,
                       std::int32_t thread_uid) {
    if (dispatch_pc == 0x08A5FDBCu) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::cerr << "[tow2-title] 0x08A5FDBC caller ra="
                      << psprecomp::hex32(ctx.gpr[31])
                      << " a0=" << psprecomp::hex32(ctx.gpr[4])
                      << " a1=" << psprecomp::hex32(ctx.gpr[5])
                      << " uid=" << thread_uid << "\n";
        }
    }
    if (dispatch_pc == 0x08814848u || dispatch_pc == 0x08817AD0u) {
        std::cerr << "[tow2-scene-caller] pc="
                  << psprecomp::hex32(dispatch_pc)
                  << " ra=" << psprecomp::hex32(ctx.gpr[31])
                  << " a0=" << psprecomp::hex32(ctx.gpr[4])
                  << " a1=" << psprecomp::hex32(ctx.gpr[5])
                  << " uid=" << thread_uid << "\n";
    }
    if (dispatch_pc == 0x0881C224u && g_asset_owner == 0u) {
        g_asset_owner = ctx.gpr[4];
        std::cerr << "[tow2-asset-owner] this="
                  << psprecomp::hex32(g_asset_owner)
                  << " ra=" << psprecomp::hex32(ctx.gpr[31])
                  << " uid=" << thread_uid << "\n";
    }
    if (dispatch_pc == 0x08817298u || dispatch_pc == 0x08813874u ||
        dispatch_pc == 0x08814460u || dispatch_pc == 0x0881460Cu ||
        dispatch_pc == 0x08813F00u) {        static std::uint64_t seen = 0;
        if (seen < 40u) {
            ++seen;
            std::cerr << "[tow2-scene-caller] pc="
                      << psprecomp::hex32(dispatch_pc)
                      << " ra=" << psprecomp::hex32(ctx.gpr[31])
                      << " a0=" << psprecomp::hex32(ctx.gpr[4])
                      << " a1=" << psprecomp::hex32(ctx.gpr[5])
                      << " uid=" << thread_uid << "\n";
        }
    }
    if (std::getenv("PSPRECOMP_TOW2_DRIVE_CONTAINER") != nullptr) {
        if (dispatch_pc == 0x088206D8u && g_drive_container_countdown < 0) {
            g_drive_container_countdown = 20000;
        }
        if (g_drive_container_countdown > 0) {
            --g_drive_container_countdown;
        } else if (g_drive_container_countdown == 0) {
            g_drive_container_countdown = -1;
            constexpr std::uint32_t kVtable = 0x08C44B08u;
            std::uint32_t container = 0u;
            for (std::uint32_t address = 0x08D15000u; address < 0x0A000000u;
                 address += 4u) {
                if (!runtime.memory().contains(address, 4u)) continue;
                if (runtime.memory().load32(address) == kVtable) {
                    container = address;
                    break;
                }
            }
            std::cerr << "[tow2-drive] container=" << psprecomp::hex32(container)
                      << "\n";
            if (container != 0u) {
                const std::uint32_t method =
                    runtime.memory().load32(kVtable + 4u);
                std::cerr << "[tow2-drive] method(+4)="
                          << psprecomp::hex32(method) << " state="
                          << psprecomp::hex32(
                                 runtime.memory().load32(container + 1228u))
                          << "\n";
                psprecomp::AllegrexContext drive_ctx = ctx;
                drive_ctx.set_gpr(4, container);
                drive_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool drive_ok = run_isolated_aot(
                    runtime, drive_ctx, method, kIsolatedReturnSentinel,
                    500000, "container-load");
                std::cerr << "[tow2-drive] ok=" << (drive_ok ? 1 : 0)
                          << " ret=" << psprecomp::hex32(drive_ctx.gpr[2])
                          << " state="
                          << psprecomp::hex32(
                                 runtime.memory().load32(container + 1228u))
                          << "\n";
            }
        }
    }
}

void scene_watch_hook(psprecomp::Runtime &runtime,
                      psprecomp::AllegrexContext &,
                      std::uint32_t dispatch_pc,
                      std::int32_t thread_uid) {
    const std::uint32_t scene =
        runtime.memory().contains(0x08D14F2Cu, 4u)
            ? runtime.memory().load32(0x08D14F2Cu) : 0u;
    std::uint32_t value = 0xFFFFFFFFu;
    if (scene != 0u && runtime.memory().contains(scene + 4u, 2u)) {
        value = runtime.memory().load16(scene + 4u);
    }
    if (scene != g_scene_watch_address || value != g_scene_watch_value) {
        std::cerr << "[tow2-scene-watch] scene="
                  << psprecomp::hex32(scene)
                  << " value=" << psprecomp::hex32(value)
                  << " dispatch=" << psprecomp::hex32(dispatch_pc)
                  << " uid=" << thread_uid << "\n";
        g_scene_watch_address = scene;
        g_scene_watch_value = value;
    }
}

void log_scene_state(psprecomp::Runtime &runtime, const char *tag) {
    const std::uint32_t scene =
        runtime.memory().contains(0x08D14F2Cu, 4u)
            ? runtime.memory().load32(0x08D14F2Cu) : 0u;
    std::uint32_t id = 0xFFFFFFFFu;
    if (scene != 0u && runtime.memory().contains(scene + 4u, 2u)) {
        id = runtime.memory().load16(scene + 4u);
    }
    std::cerr << "[tow2-title] " << tag << " scene="
              << psprecomp::hex32(scene)
              << " id=" << psprecomp::hex32(id) << "\n";
}

void title_state_thread_entry(psprecomp::Runtime &runtime,
                              psprecomp::AllegrexContext &ctx) {
    const std::uint32_t manager =
        runtime.memory().contains(0x08D14F54u, 4u)
            ? runtime.memory().load32(0x08D14F54u) : 0u;
    std::cerr << "[tow2-title] state manager=" << psprecomp::hex32(manager) << "\n";
    ctx.gpr[4] = manager;
    ctx.gpr[5] = 0u;
    ctx.gpr[31] = kThreadReturnPc;
    ctx.pc = 0x08813874u;
}

void title_thread_creator_entry(psprecomp::Runtime &,
                                psprecomp::AllegrexContext &ctx) {
    std::cerr << "[tow2-title] direct title creator object="
              << psprecomp::hex32(g_title_object) << "\n";
    ctx.gpr[4] = g_title_object;
    ctx.gpr[5] = 5u;
    ctx.gpr[31] = kThreadReturnPc;
    ctx.pc = 0x08A4B168u;
}

void title_state2_thread_entry(psprecomp::Runtime &runtime,
                               psprecomp::AllegrexContext &ctx) {
    log_scene_state(runtime, "state2");
    std::cerr << "[tow2-title] state2 mode=1\n";
    ctx.gpr[4] = 0u;
    ctx.gpr[5] = 1u;
    ctx.gpr[31] = kThreadReturnPc;
    ctx.pc = 0x08813F00u;
}



void ge_draw_stub(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::cerr << "[tow2-ge] stubbing guest draw with null GE context\n";
    }
    ctx.set_gpr(2, 0u);
    ctx.pc = ctx.gpr[31];
}

void signal_sleep_semaphore_for_vblank() {
    if (g_sleep_semaphore_uid < 0) return;
    const auto semaphore = g_semaphores.find(g_sleep_semaphore_uid);
    if (semaphore == g_semaphores.end() || semaphore->second.waiters.empty()) return;
    // PSP semaphore queues are priority-ordered: wake the highest-priority
    // waiter, not the oldest. FIFO here woke user_main (priority 63) before the
    // title mount thread (priority 17) and re-entered it out of order.
    std::size_t best = 0;
    int best_priority = std::numeric_limits<int>::max();
    const bool hold_main = g_mount_thread_uid >= 0 &&
        std::getenv("PSPRECOMP_TOW2_HOLD_MAIN_SEMA") != nullptr;
    for (std::size_t i = 0; i < semaphore->second.waiters.size(); ++i) {
        const auto thread =
            g_scheduler.threads.find(semaphore->second.waiters[i].uid);
        if (hold_main && thread != g_scheduler.threads.end() &&
            thread->second.name == "user_main") {
            continue;
        }
        const int priority = thread == g_scheduler.threads.end()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(thread->second.priority);
        if (priority < best_priority) {
            best_priority = priority;
            best = i;
        }
    }
    if (best_priority == std::numeric_limits<int>::max()) return;
    SemaphoreWaiter waiter = semaphore->second.waiters[best];
    semaphore->second.waiters.erase(semaphore->second.waiters.begin() +
                                    static_cast<std::ptrdiff_t>(best));
    psprecomp::AllegrexContext context = waiter.context;
    context.set_gpr(2, 0u);
    enqueue_thread(waiter.uid, context);
}

void starvation_preempt(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    (void)runtime;
    // Approximate execution-driven time and model the vblank/timer signal the
    // title's SleepSema wait depends on.
    static const bool vblank_sema =
        std::getenv("PSPRECOMP_TOW2_NO_VBLANK_SEMA") == nullptr;
    if (vblank_sema) {
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

    if (thread_diag()) {
        std::cerr << "[tow2] create thread uid=" << uid << " name=\"" << record.name
                  << "\" entry=" << psprecomp::hex32(entry)
                  << " priority=" << priority
                  << " stack=" << psprecomp::hex32(stack_bottom) << "-"
                  << psprecomp::hex32(stack_top) << "\n";
    }

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
    // at a module-info block whose +0x37C field is the module UID. Propagate
    // the same block into each new thread's kernel context so its
    // _getmodreent() lookup does not fall back with "no reent structure".
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

    if (thread_diag()) {
        std::cerr << "[tow2] start thread uid=" << uid << " name=\"" << thread.name
                  << "\" entry=" << psprecomp::hex32(thread.entry)
                  << " priority=" << thread.priority << " caller=" << caller_uid
                  << " caller_priority=" << caller_priority << "\n";
    }

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

bool run_isolated_aot(psprecomp::Runtime &runtime,
                      psprecomp::AllegrexContext &ctx, std::uint32_t entry,
                      std::uint32_t return_sentinel, int max_steps,
                      const char *tag) {
    ctx.pc = entry;
    ctx.set_gpr(31, return_sentinel);

    std::array<std::uint32_t, 12> recent{};
    std::size_t recent_count = 0;
    auto record = [&](std::uint32_t pc) {
        recent[recent_count % recent.size()] = pc;
        ++recent_count;
    };
    auto dump_recent = [&]() {
        const std::size_t shown = std::min<std::size_t>(recent_count, recent.size());
        std::cerr << "[tow2-iso] " << tag << " recent:";
        for (std::size_t i = 0; i < shown; ++i) {
            const std::size_t index =
                (recent_count - shown + i) % recent.size();
            std::cerr << " " << psprecomp::hex32(recent[index]);
        }
        std::cerr << "\n[tow2-iso] " << tag << " regs: a0="
                  << psprecomp::hex32(ctx.gpr[4]) << " a1="
                  << psprecomp::hex32(ctx.gpr[5]) << " a2="
                  << psprecomp::hex32(ctx.gpr[6]) << " a3="
                  << psprecomp::hex32(ctx.gpr[7]) << " sp="
                  << psprecomp::hex32(ctx.gpr[29]) << " ra="
                  << psprecomp::hex32(ctx.gpr[31]) << "\n";
    };

    for (int step = 0; step < max_steps; ++step) {
        if (ctx.pc == return_sentinel) return true;
        record(ctx.pc);
        if (!runtime.invoke_isolated_aot(ctx.pc, ctx)) {
            std::cerr << "[tow2-iso] " << tag
                      << " unresolved pc=" << psprecomp::hex32(ctx.pc)
                      << " step=" << step
                      << " registered="
                      << (runtime.has_function(ctx.pc) ? 1 : 0) << "\n";
            dump_recent();
            return false;
        }
    }
    std::cerr << "[tow2-iso] " << tag << " step limit=" << max_steps
              << " at pc=" << psprecomp::hex32(ctx.pc) << "\n";
    dump_recent();
    return false;
}

void delete_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == g_scheduler.current_uid) {
        // sceKernelDeleteThread is specified for other (dormant) threads, but
        // some titles use it as an exit path. Preserve that behavior.
        complete_current_thread(runtime, ctx);
        return;
    }
    const auto target = g_scheduler.threads.find(uid);
    if (target == g_scheduler.threads.end()) {
        ctx.set_gpr(2, 0x80020198u); // UNKNOWN_THID
        return;
    }

    if (std::getenv("PSPRECOMP_TOW2_MOVIE_RESULT") != nullptr &&
        target->second.name == "soundThread" && ctx.gpr[30] != 0u &&
        runtime.memory().contains(ctx.gpr[30] + 0x2Cu, 4u)) {
        std::cerr << "[tow2] movie result="
                  << psprecomp::hex32(runtime.memory().load32(ctx.gpr[30] + 0x2Cu))
                  << " fp=" << psprecomp::hex32(ctx.gpr[30]) << "\n";
    }
    if (thread_diag()) {
        std::cerr << "[tow2] delete thread uid=" << uid << " name=\""
                  << target->second.name << "\" pc="
                  << psprecomp::hex32(target->second.context.pc) << "\n";
    }
    if (std::getenv("PSPRECOMP_TOW2_TITLE_TRIGGER") != nullptr &&
        target->second.name == "soundThread") {
        g_movie_done = true;
        const std::uint32_t title_object =
            runtime.memory().contains(0x08D14F10u, 4u)
                ? runtime.memory().load32(0x08D14F10u) : 0u;
        g_title_object = title_object;
        const std::uint32_t scene_object =
            runtime.memory().contains(0x08D14F2Cu, 4u)
                ? runtime.memory().load32(0x08D14F2Cu) : 0u;
        std::uint32_t scene_id = 0xFFFFFFFFu;
        if (scene_object != 0u &&
            runtime.memory().contains(scene_object + 4u, 2u)) {
            scene_id = runtime.memory().load16(scene_object + 4u);
        }
        std::cerr << "[tow2-title] scene object="
                  << psprecomp::hex32(scene_object)
                  << " id=" << psprecomp::hex32(scene_id) << "\n";
        if (std::getenv("PSPRECOMP_TOW2_SCAN_FN_PTR") != nullptr) {
            constexpr std::uint32_t kActivationPc = 0x08970A00u;
            for (std::uint32_t address = 0x08800000u;
                 address < 0x0A000000u; address += 4u) {
                if (!runtime.memory().contains(address, 4u)) continue;
                if (runtime.memory().load32(address) == kActivationPc) {
                    std::cerr << "[tow2-fnptr] 0x08970A00 at="
                              << psprecomp::hex32(address) << "\n";
                }
            }
        }
        if (scene_object != 0u &&
            runtime.memory().contains(scene_object + 0x3C80u, 1u)) {
            const std::uint8_t active =
                runtime.memory().load8(scene_object + 0x3C80u);
            std::cerr << "[tow2-title] scene active="
                      << static_cast<unsigned>(active) << "\n";
            if (active == 0u) {
                runtime.memory().store8(scene_object + 0x3C80u, 1u);
                std::cerr << "[tow2-title] forced scene active=1\n";
            }
        }
        if (const char *scene_id_text = std::getenv("PSPRECOMP_TOW2_SCENE_ID")) {
            if (scene_object != 0u &&
                runtime.memory().contains(scene_object + 4u, 2u)) {
                char *end = nullptr;
                const unsigned long value = std::strtoul(scene_id_text, &end, 0);
                if (end != scene_id_text && *end == '\0' && value < 28u) {
                    runtime.memory().store16(
                        scene_object + 4u, static_cast<std::uint16_t>(value));
                    std::cerr << "[tow2-title] forced scene id=" << value
                              << "\n";
                }
            }
        }
        if (std::getenv("PSPRECOMP_TOW2_TITLE_VTABLE") != nullptr &&
            title_object != 0u &&
            runtime.memory().contains(title_object + 12032u + 4u, 4u)) {
            const std::uint32_t container = title_object + 12032u;
            const std::uint32_t vtable = runtime.memory().load32(container);
            std::cerr << "[tow2-vt] container=" << psprecomp::hex32(container)
                      << " vtable=" << psprecomp::hex32(vtable)
                      << " state="
                      << psprecomp::hex32(
                             runtime.memory().load32(container + 1228u))
                      << "\n";
            std::cerr << "[tow2-flags] b68="
                      << static_cast<unsigned>(runtime.memory().load8(0x08CAE068u))
                      << " b70="
                      << static_cast<unsigned>(runtime.memory().load8(0x08CAE070u))
                      << " w60="
                      << psprecomp::hex32(runtime.memory().load32(0x08CAE060u))
                      << "\n";
            for (std::uint32_t index = 0; index < 32u; ++index) {
                if (!runtime.memory().contains(vtable + index * 4u, 4u)) break;
                std::cerr << "[tow2-vt] +" << index * 4u << " = "
                          << psprecomp::hex32(
                                 runtime.memory().load32(vtable + index * 4u))
                          << "\n";
            }
        }
        if (std::getenv("PSPRECOMP_TOW2_SCENE_START") != nullptr) {
            // Real scene-change path used by the title script at 0x0897C684:
            //   scene = 0x08AC5428();
            //   0x08814628(scene, 1);
            psprecomp::AllegrexContext manager_ctx = ctx;
            manager_ctx.set_gpr(4, 0u);
            manager_ctx.set_gpr(29, ctx.gpr[29] - 0x1000u);
            const bool manager_ok = run_isolated_aot(
                runtime, manager_ctx, 0x08AC5428u, kIsolatedReturnSentinel,
                200000, "scene-manager");
            const std::uint32_t scene_manager = manager_ctx.gpr[2];
            std::cerr << "[tow2-scene] manager ok=" << (manager_ok ? 1 : 0)
                      << " value=" << psprecomp::hex32(scene_manager) << "\n";
            psprecomp::AllegrexContext start_ctx = ctx;
            start_ctx.set_gpr(4, scene_manager);
            start_ctx.set_gpr(29, ctx.gpr[29] - 0x4000u);
            if (std::getenv("PSPRECOMP_TOW2_SCENE_REG") != nullptr) {
                psprecomp::AllegrexContext reg_ctx = ctx;
                reg_ctx.set_gpr(4, scene_manager);
                reg_ctx.set_gpr(5, 0u);
                reg_ctx.set_gpr(6, 0u);
                reg_ctx.set_gpr(7, scene_manager); // candidate scene object
                reg_ctx.set_gpr(8, 0u);
                reg_ctx.set_gpr(9, 0u);
                reg_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool reg_ok = run_isolated_aot(
                    runtime, reg_ctx, 0x08814AECu, kIsolatedReturnSentinel,
                    200000, "scene-reg");
                std::cerr << "[tow2-scene] register ok=" << (reg_ok ? 1 : 0)
                          << " object=" << psprecomp::hex32(scene_manager)
                          << " fields=" << psprecomp::hex32(
                                 runtime.memory().load32(scene_manager + 15340u))
                          << "," << psprecomp::hex32(
                                 runtime.memory().load32(scene_manager + 15348u))
                          << "\n";
            }
            for (std::uint32_t mode = 0; mode < 3u; ++mode) {
                start_ctx.set_gpr(4, scene_manager);
                start_ctx.set_gpr(5, mode);
                const bool start_ok = run_isolated_aot(
                    runtime, start_ctx, 0x08814628u, kIsolatedReturnSentinel,
                    2000000, "scene-start");
                std::cerr << "[tow2-scene] start(" << mode
                          << ") ok=" << (start_ok ? 1 : 0)
                          << " ret=" << psprecomp::hex32(start_ctx.gpr[2])
                          << " pc=" << psprecomp::hex32(start_ctx.pc) << "\n";
                log_scene_state(runtime, "after-start");
            }
        }

        if (title_object != 0u &&
            runtime.memory().contains(title_object + 0x4F68u, 1u)) {
            runtime.memory().store8(title_object + 0x4F68u, 0u);
            std::cerr << "[tow2-title] cleared movie flag object="
                      << psprecomp::hex32(title_object) << "\n";
        }
        if (title_object != 0u) {
            runtime.register_function(0x08A76ED8u, &ge_draw_stub,
                                      "tow2_ge_draw_stub_trigger");
            if (runtime.memory().contains(0x08D030D0u, 0x34u)) {
                const std::uint32_t scratch =
                    (g_partitions.next_address + 0xFFu) & ~0xFFu;
                if (scratch + 0x40u <= g_scheduler.next_stack_top &&
                    runtime.memory().contains(scratch, 0x40u)) {
                    for (std::uint32_t off = 0; off < 0x40u; off += 4u) {
                        runtime.memory().store32(scratch + off, 0u);
                    }
                    g_partitions.next_address = scratch + 0x40u;
                    runtime.memory().store32(0x08D03100u, scratch);
                    std::cerr << "[tow2-title] seeded GE context="
                              << psprecomp::hex32(scratch) << "\n";
                }
            }
            psprecomp::AllegrexContext load_ctx = ctx;
            load_ctx.set_gpr(4, title_object);
            load_ctx.set_gpr(29, ctx.gpr[29] - 0x1000u);
            const bool load_ok = run_isolated_aot(runtime, load_ctx, 0x08A4B0FCu,
                                                  kIsolatedReturnSentinel, 200000,
                                                  "load");
            std::cerr << "[tow2-title] load ok=" << (load_ok ? 1 : 0)
                      << " object=" << psprecomp::hex32(title_object) << "\n";
            if (std::getenv("PSPRECOMP_TOW2_TITLE_ARC_OPEN") != nullptr) {
                psprecomp::AllegrexContext arc_ctx = ctx;
                arc_ctx.set_gpr(4, 0x08C23BB0u); // "title/title.arc"
                arc_ctx.set_gpr(5, 0u);
                arc_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool open_ok = run_isolated_aot(
                    runtime, arc_ctx, 0x08A7384Cu, kIsolatedReturnSentinel,
                    200000, "open");
                std::cerr << "[tow2-title] title.arc open ok="
                          << (open_ok ? 1 : 0)
                          << " ret=" << psprecomp::hex32(arc_ctx.gpr[2])
                          << "\n";
            }
            if (std::getenv("PSPRECOMP_TOW2_TITLE_MOUNT") != nullptr) {
                // 0x088206D8 is the title-screen archive-mount routine
                // ("title/title.arc" -> mount point "TitleArchive/"). It is
                // only referenced from a relocated data pointer, so it never
                // runs through the normal boot path in the native port.
                psprecomp::AllegrexContext mount_ctx = ctx;
                mount_ctx.set_gpr(4, 0u);
                mount_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool mount_ok = run_isolated_aot(
                    runtime, mount_ctx, 0x088206D8u, kIsolatedReturnSentinel,
                    200000, "mount");
                std::cerr << "[tow2-title] mount ok=" << (mount_ok ? 1 : 0)
                          << " ret=" << psprecomp::hex32(mount_ctx.gpr[2])
                          << "\n";
            }
            if (std::getenv("PSPRECOMP_TOW2_TITLE_MOUNT_THREAD") != nullptr) {
                // The mount routine waits on a worker semaphore, so it cannot
                // run inside the isolated dispatcher (which never lets other
                // guest threads run). Start it as a real guest thread instead.
                psprecomp::AllegrexContext mount_thread_ctx = ctx;
                mount_thread_ctx.set_gpr(4, 0u);
                mount_thread_ctx.set_gpr(5, 0x088206D8u);
                mount_thread_ctx.set_gpr(6, 17u);
                mount_thread_ctx.set_gpr(7, 0x4000u);
                mount_thread_ctx.set_gpr(8, 0u);
                create_thread(runtime, mount_thread_ctx);
                const std::int32_t mount_uid =
                    static_cast<std::int32_t>(mount_thread_ctx.gpr[2]);
                if (mount_uid > 0) {
                    psprecomp::AllegrexContext start_ctx = ctx;
                    start_ctx.set_gpr(4, static_cast<std::uint32_t>(mount_uid));
                    start_ctx.set_gpr(5, 0u);
                    start_ctx.set_gpr(6, 0u);
                    start_thread(runtime, start_ctx);
                    std::cerr << "[tow2-title] mount thread uid=" << mount_uid
                              << "\n";
                    g_mount_thread_uid = mount_uid;
                    if (std::getenv("PSPRECOMP_TOW2_DRIVE_CONTAINER") != nullptr) {
                        g_drive_container_countdown = 20000;
                    }
                    if (std::getenv("PSPRECOMP_TOW2_HOLD_USER_MAIN") != nullptr) {
                        for (auto &[thread_uid, thread] : g_scheduler.threads) {
                            if (thread.name != "user_main") continue;
                            thread.state = ThreadState::Delayed;
                            thread.delay_until_us =
                                std::numeric_limits<std::uint64_t>::max();
                            thread.delay_sequence = g_scheduler.next_sequence++;
                            g_scheduler.ready.erase(
                                std::remove_if(
                                    g_scheduler.ready.begin(),
                                    g_scheduler.ready.end(),
                                    [thread_uid = thread_uid](const ReadyEntry &entry) {
                                        return entry.uid == thread_uid;
                                    }),
                                g_scheduler.ready.end());
                            std::cerr << "[tow2-sched] held user_main uid="
                                      << thread_uid << " until mount completes\n";
                        }
                    }
                } else {
                    std::cerr << "[tow2-title] mount thread create failed="
                              << psprecomp::hex32(mount_thread_ctx.gpr[2])
                              << "\n";
                }
            }
            if (std::getenv("PSPRECOMP_TOW2_VFS_FIND") != nullptr) {
                // Probe the engine VFS mount table after the title mount:
                // manager = 0x08AC5338(), then 0x08A59378(manager, path).
                psprecomp::AllegrexContext mgr_ctx = ctx;
                mgr_ctx.set_gpr(29, ctx.gpr[29] - 0x1000u);
                const bool mgr_ok = run_isolated_aot(
                    runtime, mgr_ctx, 0x08AC5338u, kIsolatedReturnSentinel,
                    200000, "manager");
                const std::uint32_t manager = mgr_ctx.gpr[2];
                std::cerr << "[tow2-vfs] manager ok=" << (mgr_ok ? 1 : 0)
                          << " value=" << psprecomp::hex32(manager) << "\n";
                psprecomp::AllegrexContext find_ctx = ctx;
                find_ctx.set_gpr(4, manager);
                find_ctx.set_gpr(5, 0x08C239A4u); // "TitleArchive/water_02.ppt"
                find_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool find_ok = run_isolated_aot(
                    runtime, find_ctx, 0x08A59378u, kIsolatedReturnSentinel,
                    200000, "find");
                std::cerr << "[tow2-vfs] find ok=" << (find_ok ? 1 : 0)
                          << " ret=" << psprecomp::hex32(find_ctx.gpr[2])
                          << "\n";
            }
            if (std::getenv("PSPRECOMP_TOW2_EZBIND_PROBE") != nullptr) {
                // Drive the engine's own synchronous open wrapper
                // (0x08A7384C) with the title archive path. Opening the file
                // through guest code proves the path is reachable; the host
                // then parses the container natively in inspect_ezbind_archive.
                constexpr char kTitleArchive[] =
                    "disc0:/PSP_GAME/USRDIR/title/title.arc";
                const std::uint32_t text = ctx.gpr[29] - 0x80u;
                bool writable = runtime.memory().contains(text, sizeof(kTitleArchive));
                for (std::size_t i = 0; writable && i < sizeof(kTitleArchive); ++i) {
                    runtime.memory().store8(text + static_cast<std::uint32_t>(i),
                                            static_cast<std::uint8_t>(kTitleArchive[i]));
                }
                psprecomp::AllegrexContext open_ctx = ctx;
                open_ctx.set_gpr(4, text);
                open_ctx.set_gpr(5, 1u);
                open_ctx.set_gpr(6, 0u);
                open_ctx.set_gpr(29, ctx.gpr[29] - 0x2000u);
                const bool open_ok = run_isolated_aot(
                    runtime, open_ctx, 0x08A7384Cu, kIsolatedReturnSentinel,
                    200000, "ezbind-open");
                std::cerr << "[tow2-title] title.arc guest open ok="
                          << (open_ok ? 1 : 0)
                          << " fd=" << psprecomp::hex32(open_ctx.gpr[2]) << "\n";
            }

            if (std::getenv("PSPRECOMP_TOW2_TITLE_MOUNT_THREAD") == nullptr) {
            psprecomp::AllegrexContext creator_ctx{};
            creator_ctx.gpr[4] = 0u;
            creator_ctx.gpr[5] = kTitleCreatorThread;
            creator_ctx.gpr[6] = 17u;
            creator_ctx.gpr[7] = 0x4000u;
            creator_ctx.gpr[8] = 0u;
            create_thread(runtime, creator_ctx);
            const std::int32_t creator_uid =
                static_cast<std::int32_t>(creator_ctx.gpr[2]);
            if (creator_uid > 0) {
                psprecomp::AllegrexContext start_ctx{};
                start_ctx.gpr[4] = static_cast<std::uint32_t>(creator_uid);
                start_ctx.gpr[5] = 0u;
                start_ctx.gpr[6] = 0u;
                start_thread(runtime, start_ctx);
                std::cerr << "[tow2-title] creator uid=" << creator_uid
                          << " object=" << psprecomp::hex32(title_object) << "\n";
            } else {
                std::cerr << "[tow2-title] creator create failed="
                          << psprecomp::hex32(creator_ctx.gpr[2]) << "\n";
            }

            psprecomp::AllegrexContext state_ctx{};
            state_ctx.gpr[4] = 0u;
            state_ctx.gpr[5] = kTitleStateThread;
            state_ctx.gpr[6] = 17u;
            state_ctx.gpr[7] = 0x4000u;
            state_ctx.gpr[8] = 0u;
            create_thread(runtime, state_ctx);
            const std::int32_t state_uid =
                static_cast<std::int32_t>(state_ctx.gpr[2]);
            if (state_uid > 0) {
                psprecomp::AllegrexContext start_ctx{};
                start_ctx.gpr[4] = static_cast<std::uint32_t>(state_uid);
                start_ctx.gpr[5] = 0u;
                start_ctx.gpr[6] = 0u;
                start_thread(runtime, start_ctx);
                std::cerr << "[tow2-title] state uid=" << state_uid << "\n";
            } else {
                std::cerr << "[tow2-title] state create failed="
                          << psprecomp::hex32(state_ctx.gpr[2]) << "\n";
            }

            psprecomp::AllegrexContext title_ctx{};
            title_ctx.gpr[4] = 0u;
            title_ctx.gpr[5] = kTitleThreadCreator;
            title_ctx.gpr[6] = 17u;
            title_ctx.gpr[7] = 0x4000u;
            title_ctx.gpr[8] = 0u;
            create_thread(runtime, title_ctx);
            const std::int32_t title_uid =
                static_cast<std::int32_t>(title_ctx.gpr[2]);
            if (title_uid > 0) {
                psprecomp::AllegrexContext start_ctx{};
                start_ctx.gpr[4] = static_cast<std::uint32_t>(title_uid);
                start_ctx.gpr[5] = 0u;
                start_ctx.gpr[6] = 0u;
                start_thread(runtime, start_ctx);
                std::cerr << "[tow2-title] title uid=" << title_uid << "\n";
            } else {
                std::cerr << "[tow2-title] title create failed="
                          << psprecomp::hex32(title_ctx.gpr[2]) << "\n";
            }

            psprecomp::AllegrexContext state2_ctx{};
            state2_ctx.gpr[4] = 0u;
            state2_ctx.gpr[5] = kTitleState2Thread;
            state2_ctx.gpr[6] = 17u;
            state2_ctx.gpr[7] = 0x8000u;
            state2_ctx.gpr[8] = 0u;
            create_thread(runtime, state2_ctx);
            const std::int32_t state2_uid =
                static_cast<std::int32_t>(state2_ctx.gpr[2]);
            if (state2_uid > 0) {
                psprecomp::AllegrexContext start_ctx{};
                start_ctx.gpr[4] = static_cast<std::uint32_t>(state2_uid);
                start_ctx.gpr[5] = 0u;
                start_ctx.gpr[6] = 0u;
                start_thread(runtime, start_ctx);
                std::cerr << "[tow2-title] state2 uid=" << state2_uid << "\n";
            } else {
                std::cerr << "[tow2-title] state2 create failed="
                          << psprecomp::hex32(state2_ctx.gpr[2]) << "\n";
            }
            }
        }
    }

    // The target must not be scheduled again, even if the caller skipped
    // WaitThreadEnd (the media EOF path does exactly that).
    g_scheduler.ready.erase(
        std::remove_if(g_scheduler.ready.begin(), g_scheduler.ready.end(),
                       [uid](const ReadyEntry &entry) { return entry.uid == uid; }),
        g_scheduler.ready.end());

    // Serve anyone already waiting for the target's exit before dropping it.
    if (const auto waiters = g_scheduler.end_waiters.find(uid);
        waiters != g_scheduler.end_waiters.end()) {
        for (ThreadWaiter &waiter : waiters->second) {
            waiter.context.set_gpr(2, target->second.exit_status);
            if (waiter.status_pointer != 0u &&
                runtime.memory().contains(waiter.status_pointer, 4u)) {
                runtime.memory().store32(waiter.status_pointer,
                                         target->second.exit_status);
            }
            enqueue_thread(waiter.uid, waiter.context);
        }
        g_scheduler.end_waiters.erase(waiters);
    }

    g_scheduler.threads.erase(target);
    ctx.set_gpr(2, 0u);
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
        std::cerr << "[tow2] create event flag uid=" << uid << " name=\""
                  << g_event_flags[uid].name << "\" pattern="
                  << psprecomp::hex32(initial) << "\n";
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
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
        if ((waiter->mode & 0x20u) != 0u) flag.pattern &= ~waiter->bits; // WAITCLEAR
        else if ((waiter->mode & 0x10u) != 0u) flag.pattern = 0u;         // WAITCLEARALL
        psprecomp::AllegrexContext context = waiter->context;
        context.set_gpr(2, 0u);
        enqueue_thread(waiter->uid, context);
        waiter = flag.waiters.erase(waiter);
    }
}

bool signal_named_event_flag(psprecomp::Runtime &runtime, const std::string &name,
                             std::uint32_t bits) {
    for (auto &[uid, flag] : g_event_flags) {
        (void)uid;
        if (flag.name != name) continue;
        flag.pattern |= bits;
        wake_event_flag_waiters(runtime, flag);
        if (sound_diag()) {
            std::cerr << "[tow2-sound] signal \"" << name << "\" bits="
                      << psprecomp::hex32(bits) << " pattern="
                      << psprecomp::hex32(flag.pattern) << "\n";
        }
        return true;
    }
    return false;
}

bool reset_named_event_flag(psprecomp::Runtime &runtime, const std::string &name,
                            std::uint32_t pattern) {
    for (auto &[uid, flag] : g_event_flags) {
        (void)uid;
        if (flag.name != name) continue;
        flag.pattern = pattern;
        wake_event_flag_waiters(runtime, flag);
        if (sound_diag()) {
            std::cerr << "[tow2-sound] reset \"" << name << "\" pattern="
                      << psprecomp::hex32(flag.pattern) << "\n";
        }
        return true;
    }
    return false;
}

void evf_log(const char *op, std::uint32_t uid, const EventFlagRecord &flag,
             std::uint32_t bits, std::uint32_t mode) {
    if (!sound_diag()) return;
    // Sets/clears are noisy; focus on the waits/polls.
    if (op[0] != 'w' && op[0] != 'p') return;
    const bool interesting =
        flag.name == "control_waitFlag" || flag.name == "cSoundEffect" ||
        flag.name == "cSoundMusic" || flag.name == "cSoundVoice" ||
        flag.name == "cOnMediaStream";
    if (!interesting) return;
    if (!g_mpeg_video_eof && flag.name != "control_waitFlag") return;
    static std::unordered_map<std::string, std::uint32_t> counts;
    auto &count = counts[flag.name];
    if (count >= 20u) return;
    ++count;
    std::cerr << "[tow2-sound] " << op << " uid=" << uid
              << " name=\"" << flag.name << "\" bits=" << psprecomp::hex32(bits)
              << " mode=" << psprecomp::hex32(mode)
              << " pattern=" << psprecomp::hex32(flag.pattern) << "\n";
}

void set_event_flag(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    flag->second.pattern |= ctx.gpr[5];
    evf_log("set", static_cast<std::uint32_t>(uid), flag->second, ctx.gpr[5], 0u);
    if (sound_diag() && flag->second.name == "cOnMediaStream") {
        static std::uint32_t logs = 0u;
        if (logs++ < 200u) {
            const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
            std::cerr << "[tow2-sound] cOnMediaStream set bits="
                      << psprecomp::hex32(ctx.gpr[5])
                      << " ra=" << psprecomp::hex32(ctx.gpr[31])
                      << " uid=" << g_scheduler.current_uid << " thread=\""
                      << (current != g_scheduler.threads.end() ? current->second.name : "?")
                      << "\" pattern=" << psprecomp::hex32(flag->second.pattern) << "\n";
        }
    }
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
    evf_log("clear", static_cast<std::uint32_t>(uid), flag->second, ctx.gpr[5], 0u);
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
    evf_log("wait", static_cast<std::uint32_t>(uid), flag->second, ctx.gpr[5], ctx.gpr[6]);
    if (event_flag_satisfied(flag->second.pattern, waiter)) {
        if (waiter.output_pointer != 0u &&
            runtime.memory().contains(waiter.output_pointer, 4u)) {
            runtime.memory().store32(waiter.output_pointer, flag->second.pattern);
        }
        if ((waiter.mode & 0x20u) != 0u) flag->second.pattern &= ~waiter.bits;
        else if ((waiter.mode & 0x10u) != 0u) flag->second.pattern = 0u;
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

void poll_event_flag(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t bits = ctx.gpr[5];
    const std::uint32_t mode = ctx.gpr[6];
    const std::uint32_t out_pointer = ctx.gpr[7];
    const auto flag = g_event_flags.find(uid);
    if (flag == g_event_flags.end()) {
        ctx.set_gpr(2, 0x8002019Au);
        return;
    }
    if (bits == 0u) {
        ctx.set_gpr(2, 0x800201B1u); // EVF_ILPAT
        return;
    }
    EventFlagWaiter waiter{};
    waiter.bits = bits;
    waiter.mode = mode;
    evf_log("poll", static_cast<std::uint32_t>(uid), flag->second, bits, mode);
    if (sound_diag() && flag->second.name == "cOnMediaStream") {
        static bool first_object = true;
        static bool eof_object = false;
        const std::uint32_t object = ctx.gpr[18];
        if ((first_object || (g_mpeg_video_eof && !eof_object)) &&
            object != 0u && runtime.memory().contains(object, 0x30u)) {
            std::cerr << "[tow2-sound] stream object=" << psprecomp::hex32(object)
                      << " +00=" << psprecomp::hex32(runtime.memory().load32(object + 0x00u))
                      << " +04=" << psprecomp::hex32(runtime.memory().load32(object + 0x04u))
                      << " +08=" << psprecomp::hex32(runtime.memory().load32(object + 0x08u))
                      << " +0C=" << psprecomp::hex32(runtime.memory().load32(object + 0x0Cu))
                      << " +10=" << psprecomp::hex32(runtime.memory().load32(object + 0x10u))
                      << " +14=" << psprecomp::hex32(runtime.memory().load32(object + 0x14u))
                      << " +18=" << psprecomp::hex32(runtime.memory().load32(object + 0x18u))
                      << " +1C=" << psprecomp::hex32(runtime.memory().load32(object + 0x1Cu))
                      << " +20=" << psprecomp::hex32(runtime.memory().load32(object + 0x20u))
                      << " +24=" << psprecomp::hex32(runtime.memory().load32(object + 0x24u))
                      << " +28=" << psprecomp::hex32(runtime.memory().load32(object + 0x28u))
                      << " +2C=" << psprecomp::hex32(runtime.memory().load32(object + 0x2Cu))
                      << "\n";
            first_object = false;
            if (g_mpeg_video_eof) eof_object = true;
        }
    }
    if (sound_diag() && g_mpeg_video_eof && flag->second.name == "cOnMediaStream") {
        static std::uint32_t logs = 0u;
        if (logs++ < 100u) {
            const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
            std::cerr << "[tow2-sound] cOnMediaStream poll bits="
                      << psprecomp::hex32(bits) << " mode=" << psprecomp::hex32(mode)
                      << " ra=" << psprecomp::hex32(ctx.gpr[31])
                      << " uid=" << g_scheduler.current_uid << " thread=\""
                      << (current != g_scheduler.threads.end() ? current->second.name : "?")
                      << "\" pattern=" << psprecomp::hex32(flag->second.pattern) << "\n";
        }
    }
    // Diagnostic: some media consumers clear the event bits on the first poll
    // and then keep polling. Re-publish the event while probing the title gate.
    if (std::getenv("PSPRECOMP_TOW2_FORCE_MEDIA_EVENTS") != nullptr &&
        g_mpeg_video_eof && flag->second.name == "cOnMediaStream" && bits == 0x0Fu) {
        flag->second.pattern |= 0x0Fu;
    }
    if (!event_flag_satisfied(flag->second.pattern, waiter)) {
        if (out_pointer != 0u && runtime.memory().contains(out_pointer, 4u)) {
            runtime.memory().store32(out_pointer, flag->second.pattern);
        }
        ctx.set_gpr(2, 0x800201AFu); // EVF_COND (poll did not match)
        return;
    }
    if (out_pointer != 0u && runtime.memory().contains(out_pointer, 4u)) {
        runtime.memory().store32(out_pointer, flag->second.pattern);
    }
    if ((mode & 0x20u) != 0u) flag->second.pattern &= ~bits;
    else if ((mode & 0x10u) != 0u) flag->second.pattern = 0u;
    ctx.set_gpr(2, 0u);
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
    if (!name.empty()) {
        std::vector<std::uint8_t> bytes(name.begin(), name.end());
        runtime.memory().copy_in(status_pointer + 4u, bytes);
    }
    runtime.memory().store32(status_pointer + 36u, 0u);
    runtime.memory().store32(status_pointer + 40u, flag->second.pattern);
    runtime.memory().store32(status_pointer + 44u, flag->second.pattern);
    runtime.memory().store32(status_pointer + 48u,
                             static_cast<std::uint32_t>(flag->second.waiters.size()));
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

// ---------------------------------------------------------------------------
// IoFileMgrForUser
// ---------------------------------------------------------------------------

bool is_device_root(const std::string &psp_path) {
    const auto colon = psp_path.find(':');
    if (colon == std::string::npos) return false;
    std::string rest = psp_path.substr(colon + 1u);
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
        rest.erase(rest.begin());
    }
    return rest.empty();
}

const tow2::EzbindArchive *find_ezbind_archive(const std::string &psp_path) {
    const auto it = g_ezbind_archives.find(psp_path);
    return it == g_ezbind_archives.end() ? nullptr : &it->second;
}

void inspect_ezbind_archive(const std::filesystem::path &host_path,
                            const std::string &psp_path) {
    if (host_path.extension() != ".arc") return;
    if (g_ezbind_archives.contains(psp_path)) return;

    tow2::EzbindArchive archive;
    std::string error;
    if (!archive.load(host_path, &error)) {
        if (file_diag()) {
            std::cerr << "[tow2][ezbind] skip \"" << psp_path << "\": " << error
                      << "\n";
        }
        return;
    }

    std::cerr << "[tow2][ezbind] archive=\"" << psp_path << "\" version="
              << archive.version() << " entries=" << archive.entries().size()
              << "\n";
    for (const auto &entry : archive.entries()) {
        std::cerr << "[tow2][ezbind]   " << entry.name << " off="
                  << entry.data_offset << " size=" << entry.size << " hash="
                  << psprecomp::hex32(entry.hash) << "\n";
    }
    g_ezbind_archives.emplace(psp_path, std::move(archive));
}

std::filesystem::path umd_image_path(psprecomp::Runtime &runtime) {    if (const char *configured = std::getenv("PSPRECOMP_TOW2_ISO");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    return runtime.game_root().parent_path() / "tow2.iso";
}

std::int32_t open_host_file(psprecomp::Runtime &runtime, const std::string &psp_path,
                            std::uint32_t flags) {
    if (is_device_root(psp_path)) {
        const std::filesystem::path image = umd_image_path(runtime);
        std::error_code device_error;
        if (!std::filesystem::is_regular_file(image, device_error)) {
            if (file_diag()) {
                std::cerr << "[tow2][io] device ENOENT \"" << psp_path << "\" -> "
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
            std::cerr << "[tow2][io] open device fd=" << fd
                      << " image=\"" << image.string() << "\" size="
                      << g_files[fd].size << "\n";
        }
        return fd;
    }

    const std::filesystem::path host_path = runtime.translate_path(psp_path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(host_path, error)) {
        if (file_diag()) {
            std::cerr << "[tow2][io] open ENOENT \"" << psp_path << "\" -> "
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
    if (host_path.extension() == ".pmf") {
        g_pmf_host_path = host_path.string();
        g_pmf_packet_count =
            static_cast<std::uint32_t>(handle.size / 2048u);
    }

    const std::int32_t fd = g_next_fd++;
    g_files.emplace(fd, std::move(handle));
    if (file_diag()) {
        std::cerr << "[tow2][io] open fd=" << fd << " size=" << g_files[fd].size
                  << " \"" << psp_path << "\"\n";
    }
    inspect_ezbind_archive(host_path, psp_path);
    return fd;
}

void io_open(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    const std::int32_t fd = open_host_file(runtime, path, ctx.gpr[5]);
    ctx.set_gpr(2, static_cast<std::uint32_t>(fd));
    if (file_diag() && fd >= 0) {
        std::cerr << "[tow2][io] open-ra fd=" << fd
                  << " ra=" << psprecomp::hex32(ctx.gpr[31])
                  << " uid=" << g_scheduler.current_uid
                  << " path=\"" << path << "\"\n";
    }
}

void io_open_async(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    io_open(runtime, ctx);
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

    // The raw UMD device is addressed in 2048-byte sectors by this engine:
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
    if (file_diag() && g_read_diag_count < 24u) {
        ++g_read_diag_count;
        const auto current = g_scheduler.threads.find(g_scheduler.current_uid);
        std::cerr << "[tow2][io] read fd=" << fd << " request=" << size
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
        std::cerr << "[tow2][io] lseek fd=" << fd << " offset=" << offset
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
        std::cerr << "[tow2][io] devctl cmd=" << psprecomp::hex32(ctx.gpr[5])
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

// ---------------------------------------------------------------------------
// ModuleMgrForUser
// ---------------------------------------------------------------------------

void load_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t path_pointer = ctx.gpr[4];
    std::string path;
    if (path_pointer != 0u && runtime.memory().contains(path_pointer, 1u)) {
        path = runtime.memory().read_c_string(path_pointer, 1024u);
    }
    const std::int32_t uid = g_next_module_uid++;
    g_loaded_modules.emplace(uid, LoadedModule{path});
    if (module_diag()) {
        std::cerr << "[tow2][module] load uid=" << uid << " path=\"" << path
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
        std::cerr << "[tow2][module] start uid=" << uid << "\n";
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

// Load and register the plain-ELF libfile.prx secondary module.
void load_libfile_module(psprecomp::Runtime &runtime, const std::filesystem::path &path) {
    if (g_libfile_loaded) return;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        std::cerr << "[tow2][module] libfile.prx not found: " << path.string() << "\n";
        return;
    }
    auto elf = psprecomp::Elf32Image::from_file(path);
    const auto stats = elf.load_and_relocate(runtime.memory(), kLibfileBase);
    if (stats.unsupported != 0u || stats.invalid != 0u) {
        std::cerr << "[tow2][module] libfile relocation failed: unsupported="
                  << stats.unsupported << " invalid=" << stats.invalid << "\n";
        return;
    }
    const auto module = elf.find_module_info(runtime.memory(), kLibfileBase);
    if (!module) {
        std::cerr << "[tow2][module] libfile module info not found\n";
        return;
    }
    g_libfile_gp = module->gp;
    g_libfile_stack_top = kLibfileStackTop;
    runtime.memory().zero(kLibfileStackTop - kLibfileStackSize, kLibfileStackSize);
    psprecomp::libfile_register_generated_functions(runtime);
    g_libfile_loaded = true;
    std::cerr << "[tow2][module] libfile loaded base=" << psprecomp::hex32(kLibfileBase)
              << " gp=" << psprecomp::hex32(g_libfile_gp) << "\n";
}

// ---------------------------------------------------------------------------
// MPEG/Atrac host decoding
// ---------------------------------------------------------------------------

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

// Mark one guest ringbuffer packet as consumed. The title throttles its movie
// loop on sceMpegRingbufferAvailableSize, so keeping the used count at capacity
// stalls the post-movie transition forever.
void consume_mpeg_packet(psprecomp::Runtime &runtime, std::uint32_t mpeg) {
    MpegState *state = mpeg_state_for(runtime, mpeg);
    if (state == nullptr || state->ringbuffer == 0u ||
        !runtime.memory().contains(state->ringbuffer, 48u)) {
        return;
    }
    const std::uint32_t used = runtime.memory().load32(state->ringbuffer + 12u);
    if (used > 0u) runtime.memory().store32(state->ringbuffer + 12u, used - 1u);
    const std::uint32_t read = runtime.memory().load32(state->ringbuffer + 4u);
    runtime.memory().store32(state->ringbuffer + 4u, read + 1u);
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
    auto player = std::make_unique<tow2::AtracPlayer>();
    if (!player->open(path, error)) return false;
    g_atrac_player = std::move(player);
    if (mpeg_diag())
        std::cerr << "[tow2-mpeg] opened Atrac stream, frames="
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
    auto player = std::make_unique<tow2::MpegPlayer>();
    if (!player->open(path, error)) return false;
    g_mpeg_player = std::move(player);
    if (mpeg_diag()) std::cerr << "[tow2-mpeg] opened host stream \"" << path << "\"\n";
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

// ---------------------------------------------------------------------------
// sceLibFont (minimal boot contract)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kFontLibHandle = 0x08801000u;
constexpr std::uint32_t kFontHandle = 0x08801100u;
constexpr std::uint32_t kFontGlyphScratch = 0x08800000u;

void font_new_lib(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t error_pointer = ctx.gpr[5];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, kFontLibHandle);
}

void font_open(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t error_pointer = ctx.gpr[7];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, kFontHandle);
}

void font_find_optimum(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t error_pointer = ctx.gpr[6];
    if (error_pointer != 0u && runtime.memory().contains(error_pointer, 4u)) {
        runtime.memory().store32(error_pointer, 0u);
    }
    ctx.set_gpr(2, 0u); // font index 0
}

void font_get_info(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t info_pointer = ctx.gpr[5];
    if (info_pointer == 0u || !runtime.memory().contains(info_pointer, 264u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    runtime.memory().zero(info_pointer, 264u);
    runtime.memory().store32(info_pointer + 0u, 8u);
    runtime.memory().store32(info_pointer + 4u, 8u);
    runtime.memory().store32(info_pointer + 8u, 8u);
    runtime.memory().store32(info_pointer + 12u, 0u);
    runtime.memory().store32(info_pointer + 16u, 0x08000000u);
    runtime.memory().store32(info_pointer + 20u, 0x08000000u);
    runtime.memory().store32(info_pointer + 24u, 0x08000000u);
    runtime.memory().store32(info_pointer + 28u, 0x08000000u);
    runtime.memory().store32(info_pointer + 32u, 0u);
    runtime.memory().store32(info_pointer + 36u, 0u);
    runtime.memory().store32(info_pointer + 80u, 8u);
    runtime.memory().store32(info_pointer + 84u, 8u);
    runtime.memory().store32(info_pointer + 88u, 0x10000u);
    runtime.memory().store32(info_pointer + 92u, 0u);
    runtime.memory().store8(info_pointer + 260u, 8u);
    ctx.set_gpr(2, 0u);
}

void font_get_char_info(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t info_pointer = ctx.gpr[6];
    if (info_pointer == 0u || !runtime.memory().contains(info_pointer, 60u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    runtime.memory().zero(info_pointer, 60u);
    runtime.memory().store32(info_pointer + 0u, 8u);
    runtime.memory().store32(info_pointer + 4u, 8u);
    runtime.memory().store32(info_pointer + 8u, 0u);
    runtime.memory().store32(info_pointer + 12u, 0u);
    runtime.memory().store32(info_pointer + 16u, 8u);
    runtime.memory().store32(info_pointer + 20u, 8u);
    runtime.memory().store32(info_pointer + 24u, 8u);
    runtime.memory().store32(info_pointer + 28u, 0u);
    ctx.set_gpr(2, 0u);
}

void font_get_glyph(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t image_pointer = ctx.gpr[6];
    if (image_pointer == 0u || !runtime.memory().contains(image_pointer, 24u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    runtime.memory().zero(kFontGlyphScratch, 64u);
    runtime.memory().zero(image_pointer, 24u);
    runtime.memory().store32(image_pointer + 0u, 2u);
    runtime.memory().store32(image_pointer + 12u, 0x00080008u);
    runtime.memory().store16(image_pointer + 16u, 8u);
    runtime.memory().store32(image_pointer + 20u, kFontGlyphScratch);
    ctx.set_gpr(2, 0u);
}

void font_get_char_image_rect(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t rect = ctx.gpr[6];
    if (rect == 0u || !runtime.memory().contains(rect, 4u)) {
        ctx.set_gpr(2, 0x80000108u);
        return;
    }
    runtime.memory().store16(rect + 0u, 8u); // width
    runtime.memory().store16(rect + 2u, 8u); // height
    ctx.set_gpr(2, 0u);
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

// ---------------------------------------------------------------------------
// sceAudio (silent channel bookkeeping)
// ---------------------------------------------------------------------------

void audio_block(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
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
}

void sas_output(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t output = ctx.gpr[5];
    if (output != 0u && g_sas.grain > 0) {
        const std::size_t bytes = static_cast<std::size_t>(g_sas.grain) *
            (g_sas.output_mode == 0 ? 2u : 4u);
        if (rt.memory().contains(output, bytes)) rt.memory().zero(output, bytes);
    }
    // Model the core's grain duration so this call blocks/yields instead of
    // letting the sound manager spin and starve the file/title threads.
    const std::uint32_t rate =
        g_sas.sample_rate > 0 ? static_cast<std::uint32_t>(g_sas.sample_rate) : 44100u;
    const std::uint32_t samples =
        g_sas.grain > 0 ? static_cast<std::uint32_t>(g_sas.grain) : 1024u;
    const std::uint32_t microseconds = std::max<std::uint32_t>(
        100u, (samples * 1000000u) / rate);
    // Silent SAS: report all voices ended once this grain has been rendered.
    // A new __sceSasSetKeyOn clears the flag. This is what lets the title's
    // sound manager finish an effect instead of polling it forever.
    g_sas.end_flag = 0xFFFFFFFFu;
    // The sound manager polls cSoundEffect for effect completion. The silent
    // SAS path has no voices to report finished, so publish the completion
    // bits directly after every grain. This must not be gated on the movie
    // state: title/menu effects also use this path.
    signal_named_event_flag(rt, "cSoundEffect", 0x15u);
    delay_current_thread(rt, ctx, microseconds);
    ctx.set_gpr(2, 0u);
}

// ---------------------------------------------------------------------------
// sceUtility system parameters and the non-interactive savedata dialog
// ---------------------------------------------------------------------------

void get_system_param_int(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t id = ctx.gpr[4];
    const std::uint32_t destination = ctx.gpr[5];
    std::int32_t value = 0;
    switch (id) {
    case 1: value = 1; break; // language: English
    case 2: value = 0; break; // button preference
    case 3: value = 0; break;
    case 4: value = 0; break; // parental level
    case 5: value = 1; break; // 24h time
    case 6: value = 2; break; // DD/MM/YYYY
    case 7: value = 0; break; // timezone
    case 8: value = 0; break; // standard time
    case 9: value = 0; break; // adhoc channel
    case 10: value = 0; break; // WLAN power save
    default:
        ctx.set_gpr(2, 0x80110103u);
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
    if (id != 1u) {
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
        std::cerr << "[tow2] create mbx uid=" << uid << " name=\""
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
    // The Alfasystem MemoryMutex is created with initCount=1 and then
    // locked/unlocked as if 1 were an available count rather than ownership.
    // Treating it as pre-owned deadlocks the module dispatcher. Keep this
    // profile default; PSPRECOMP_TOW2_OWNED_MUTEX=1 restores strict ownership.
    const bool unowned_on_create =
        std::getenv("PSPRECOMP_TOW2_OWNED_MUTEX") == nullptr;
    record.count = unowned_on_create ? 0 : initial;
    record.owner = (!unowned_on_create && initial > 0)
        ? g_scheduler.current_uid : -1;
    g_mutexes.emplace(uid, std::move(record));
    if (thread_diag()) {
        std::cerr << "[tow2] create mutex uid=" << uid << " name=\""
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
    std::string name = "callback";
    if (name_pointer != 0u && runtime.memory().contains(name_pointer, 1u)) {
        name = runtime.memory().read_c_string(name_pointer, 128u);
    }
    const std::int32_t uid = g_next_callback_uid++;
    g_callbacks.emplace(uid, CallbackRecord{std::move(name), function, argument,
                                            g_scheduler.current_uid});
    if (thread_diag()) {
        std::cerr << "[tow2] create callback uid=" << uid << " name=\""
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
        std::cerr << "[tow2] create sema uid=" << uid << " name=\""
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

    // Diagnostic ordering: when the title mount thread ticks the sleep
    // semaphore, keep user_main asleep so the mount and its asset dispatch
    // complete first. The vblank model still wakes the highest-priority
    // sleeper, which is the mount thread.
    if (std::getenv("PSPRECOMP_TOW2_HOLD_MAIN_SEMA") != nullptr &&
        g_mount_thread_uid >= 0 &&
        uid == g_sleep_semaphore_uid &&
        g_scheduler.current_uid == g_mount_thread_uid) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::cerr << "[tow2-sched] hold user_main: mount thread signals "
                         "sleep semaphore\n";
        }
        ctx.set_gpr(2, 0u);
        return;
    }

    auto waiter = semaphore->second.waiters.begin();
    while (waiter != semaphore->second.waiters.end()) {
        // PSP semaphores wake the highest-priority waiter that fits.
        std::size_t best = semaphore->second.waiters.size();
        int best_priority = std::numeric_limits<int>::max();
        const bool hold_main = g_mount_thread_uid >= 0 &&
            std::getenv("PSPRECOMP_TOW2_HOLD_MAIN_SEMA") != nullptr;
        for (std::size_t i = 0; i < semaphore->second.waiters.size(); ++i) {
            const SemaphoreWaiter &candidate = semaphore->second.waiters[i];
            if (semaphore->second.value < candidate.count) continue;
            const auto thread = g_scheduler.threads.find(candidate.uid);
            if (hold_main && thread != g_scheduler.threads.end() &&
                thread->second.name == "user_main") {
                continue;
            }
            const int priority = thread == g_scheduler.threads.end()
                ? std::numeric_limits<int>::max()
                : static_cast<int>(thread->second.priority);
            if (priority < best_priority) {
                best_priority = priority;
                best = i;
            }
        }
        if (best == semaphore->second.waiters.size()) break;
        SemaphoreWaiter selected = semaphore->second.waiters[best];
        semaphore->second.value -= selected.count;
        psprecomp::AllegrexContext context = selected.context;
        context.set_gpr(2, 0u);
        enqueue_thread(selected.uid, context);
        semaphore->second.waiters.erase(
            semaphore->second.waiters.begin() +
            static_cast<std::ptrdiff_t>(best));
        waiter = semaphore->second.waiters.begin();
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
    const char *text = std::getenv("PSPRECOMP_TOW2_TIMESLICE");
    if (text == nullptr || *text == '\0') return 20000u;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    return end != text && *end == '\0' ? value : 20000u;
}

void initialize_scheduler(psprecomp::Runtime &runtime) {
    g_scheduler = Scheduler{};
    g_scheduler.next_stack_top = kUserRamTop - kModuleStackSize;
    g_sleep_semaphore_uid = -1;
    g_mount_thread_uid = -1;
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
                              "tow2_thread_return");
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

void report_movie_state() {
    if (std::getenv("PSPRECOMP_TOW2_MOVIE_STOP") == nullptr) return;
    const std::uint64_t decoded =
        g_mpeg_player != nullptr ? g_mpeg_player->decoded_frames() : 0u;
    std::cerr << "[tow2-movie-stop] video_eof=" << (g_mpeg_video_eof ? 1 : 0)
              << " decoded_frames=" << decoded
              << " vcount=" << g_movie_vcount
              << " fp=" << psprecomp::hex32(g_movie_fp)
              << " +18=" << psprecomp::hex32(g_movie_counter_18)
              << " +1C=" << psprecomp::hex32(g_movie_counter_1c)
              << " +30=" << psprecomp::hex32(g_movie_counter_30)
              << " +34=" << psprecomp::hex32(g_movie_counter_34);
    if (g_mpeg_memory != nullptr) {
        std::cerr << " task_state_08D0317C="
                  << psprecomp::hex32(g_mpeg_memory->load32(0x08D0317Cu))
                  << " task_state_08D03198="
                  << psprecomp::hex32(g_mpeg_memory->load32(0x08D03198u));
    }
    std::cerr << "\n";
    std::vector<std::uint32_t> seen;
    for (const auto &[key, state] : g_mpeg_states) {
        (void)key;
        const std::uint32_t ringbuffer = state.ringbuffer;
        if (ringbuffer == 0u ||
            std::find(seen.begin(), seen.end(), ringbuffer) != seen.end()) {
            continue;
        }
        seen.push_back(ringbuffer);
        if (g_mpeg_memory == nullptr ||
            !g_mpeg_memory->contains(ringbuffer, 48u)) {
            continue;
        }
        std::cerr << "[tow2-movie-stop] ring=" << psprecomp::hex32(ringbuffer)
                  << " packets=" << psprecomp::hex32(
                         g_mpeg_memory->load32(ringbuffer + 0u))
                  << " read=" << psprecomp::hex32(
                         g_mpeg_memory->load32(ringbuffer + 4u))
                  << " written=" << psprecomp::hex32(
                         g_mpeg_memory->load32(ringbuffer + 8u))
                  << " used=" << psprecomp::hex32(
                         g_mpeg_memory->load32(ringbuffer + 12u)) << "\n";
    }
}

} // namespace

void report_threads() {
    std::cerr << "[tow2] thread states:\n";    for (const auto &[uid, thread] : g_scheduler.threads) {
        std::cerr << "  uid=" << uid << " name=\"" << thread.name
                  << "\" state=" << thread_state_name(thread.state)
                  << " priority=" << thread.priority
                  << " pc=" << psprecomp::hex32(thread.context.pc)
                  << " sp=" << psprecomp::hex32(thread.context.gpr[29]) << "\n";
    }
    std::cerr << "[tow2] ready:";
    for (const ReadyEntry &entry : g_scheduler.ready) {
        std::cerr << " " << entry.uid;
    }
    std::cerr << "\n";
    if (g_mpeg_memory != nullptr &&
        std::getenv("PSPRECOMP_TOW2_VFS_DUMP") != nullptr) {
        for (const std::uint32_t base : {0x08D4F0C0u, 0x08D4F120u}) {
            if (!g_mpeg_memory->contains(base, 0x260u)) continue;
            std::cerr << "[tow2-vfs] base=" << psprecomp::hex32(base)
                      << " head=" << psprecomp::hex32(g_mpeg_memory->load32(base))
                      << " sentinel=" << psprecomp::hex32(base + 4u)
                      << " sema="
                      << psprecomp::hex32(g_mpeg_memory->load32(base + 56u))
                      << " lock="
                      << psprecomp::hex32(g_mpeg_memory->load32(base + 576u))
                      << "\n";
        }
    }
    if (g_mpeg_memory != nullptr &&
        std::getenv("PSPRECOMP_TOW2_TITLE_VTABLE") != nullptr &&
        g_title_object != 0u &&
        g_mpeg_memory->contains(g_title_object + 12036u, 4u)) {
        const std::uint32_t container = g_title_object + 12032u;
        const std::uint32_t vtable = g_mpeg_memory->load32(container);
        std::cerr << "[tow2-vt] final container=" << psprecomp::hex32(container)
                  << " vtable=" << psprecomp::hex32(vtable)
                  << " state="
                  << psprecomp::hex32(g_mpeg_memory->load32(container + 1228u))
                  << "\n";
        for (std::uint32_t index = 0;
             index < 32u && vtable != 0u &&
             g_mpeg_memory->contains(vtable + index * 4u, 4u); ++index) {
            std::cerr << "[tow2-vt] +" << index * 4u << " = "
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(vtable + index * 4u))
                      << "\n";
        }
    }
    if (g_mpeg_memory != nullptr &&
        std::getenv("PSPRECOMP_TOW2_TITLE_VTABLE") != nullptr &&
        g_asset_owner != 0u) {
        const std::uint32_t owner = g_asset_owner;
        const std::uint32_t container = owner + 12032u;
        std::cerr << "[tow2-owner] owner=" << psprecomp::hex32(owner)
                  << " vtable=" << psprecomp::hex32(
                         g_mpeg_memory->load32(owner))
                  << "\n";
        if (g_mpeg_memory->contains(container, 1300u)) {
            const std::uint32_t cvtable = g_mpeg_memory->load32(container);
            std::cerr << "[tow2-owner] container="
                      << psprecomp::hex32(container)
                      << " vtable=" << psprecomp::hex32(cvtable)
                      << " state="
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(container + 1228u))
                      << " b1232="
                      << static_cast<unsigned>(
                             g_mpeg_memory->load8(container + 1232u))
                      << " b1233="
                      << static_cast<unsigned>(
                             g_mpeg_memory->load8(container + 1233u))
                      << " p560="
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(container + 560u))
                      << " p568="
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(container + 568u))
                      << "\n";
            for (std::uint32_t index = 0;
                 index < 24u && cvtable != 0u &&
                 g_mpeg_memory->contains(cvtable + index * 4u, 4u); ++index) {
                std::cerr << "[tow2-owner] +" << index * 4u << " = "
                          << psprecomp::hex32(
                                 g_mpeg_memory->load32(cvtable + index * 4u))
                          << "\n";
            }
        }
    }
    if (g_mpeg_memory != nullptr &&
        std::getenv("PSPRECOMP_TOW2_FIND_CONTAINER") != nullptr) {
        constexpr std::uint32_t kAssetContainerVtable = 0x08C44B08u;
        for (std::uint32_t address = 0x08D15000u; address < 0x0A000000u;
             address += 4u) {
            if (!g_mpeg_memory->contains(address, 4u)) continue;
            if (g_mpeg_memory->load32(address) != kAssetContainerVtable) continue;
            const std::uint32_t container = address;
            const std::uint32_t owner = address - 12032u;
            std::cerr << "[tow2-container] at=" << psprecomp::hex32(container)
                      << " owner=" << psprecomp::hex32(owner)
                      << " state="
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(container + 1228u))
                      << " b1232="
                      << static_cast<unsigned>(
                             g_mpeg_memory->load8(container + 1232u))
                      << " b1233="
                      << static_cast<unsigned>(
                             g_mpeg_memory->load8(container + 1233u))
                      << " p1224="
                      << psprecomp::hex32(
                             g_mpeg_memory->load32(container + 1224u))
                      << "\n";
            for (std::uint32_t index = 0; index < 32u; ++index) {
                std::cerr << "[tow2-container] +" << index * 4u << " = "
                          << psprecomp::hex32(g_mpeg_memory->load32(
                                 kAssetContainerVtable + index * 4u))
                          << "\n";
            }
            break;
        }
    }
    std::cerr << "[tow2] semaphores:\n";
    for (const auto &[uid, semaphore] : g_semaphores) {
        std::cerr << "  uid=" << uid << " name=\"" << semaphore.name
                  << "\" value=" << semaphore.value
                  << " waiters=" << semaphore.waiters.size() << "\n";
    }
    std::cerr << "[tow2] mutexes:\n";
    for (const auto &[uid, mutex] : g_mutexes) {
        std::cerr << "  uid=" << uid << " name=\"" << mutex.name
                  << "\" owner=" << mutex.owner << " count=" << mutex.count
                  << " waiters=" << mutex.waiters.size() << "\n";
    }
    std::cerr << "[tow2] mailboxes:\n";
    for (const auto &[uid, mbx] : g_mbxs) {
        std::cerr << "  uid=" << uid << " name=\"" << mbx.name
                  << "\" messages=" << mbx.messages.size()
                  << " waiters=" << mbx.waiters.size() << "\n";
    }
    std::cerr << "[tow2] event flags:\n";
    for (const auto &[uid, flag] : g_event_flags) {
        std::cerr << "  uid=" << uid << " name=\"" << flag.name
                  << "\" pattern=" << psprecomp::hex32(flag.pattern)
                  << " waiters=" << flag.waiters.size() << "\n";
    }
    report_movie_state();
}

// Diagnostic guard for the game's free() (0x08A7680C), which is the generated
// unit 313 entry 69. A block whose header pointer at block+0 is zero has never
// been linked into the allocator (the first block of a heap region that was not
// initialised), and the real free() dereferences it immediately. With
// PSPRECOMP_TOW2_SKIP_INVALID_FREE=1 the guard leaks that block instead of
// faulting so the caller can be traced further.
void free_guard(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    static const bool skip_invalid =
        std::getenv("PSPRECOMP_TOW2_SKIP_INVALID_FREE") != nullptr;
    static std::uint64_t skipped = 0;
    const std::uint32_t pointer = ctx.gpr[4];
    const bool header_ok =
        pointer != 0u && runtime.memory().contains(pointer - 64u, 8u);
    const bool invalid = header_ok && runtime.memory().load32(pointer - 64u) == 0u;
    if (skip_invalid && invalid) {
        if (skipped++ < 16u) {
            std::cerr << "[tow2-heap] skip invalid free("
                      << psprecomp::hex32(pointer) << ") block="
                      << psprecomp::hex32(pointer - 64u)
                      << " (header next=0)\n";
        }
        if (skipped == 1u && runtime.memory().contains(0x08D0460Cu, 4u)) {
            const std::uint32_t heap =
                runtime.memory().load32(0x08D0460Cu);
            const std::uint32_t region =
                (heap != 0u && runtime.memory().contains(heap + 8u, 4u))
                    ? runtime.memory().load32(heap + 8u) : 0u;
            std::cerr << "[tow2-heap] heap=" << psprecomp::hex32(heap)
                      << " region=" << psprecomp::hex32(region) << "\n";
            if (region != 0u && runtime.memory().contains(region, 0x20u)) {
                std::cerr << "[tow2-heap] region head:";
                for (std::uint32_t i = 0; i < 6u; ++i) {
                    std::cerr << " " << psprecomp::hex32(
                        runtime.memory().load32(region + i * 4u));
                }
                std::cerr << "\n";
            }
            for (std::uint32_t address = pointer - 0x80u;
                 address < pointer + 0x40u; address += 0x10u) {
                if (!runtime.memory().contains(address, 0x10u)) continue;
                std::cerr << "[tow2-heap] " << psprecomp::hex32(address) << ": "
                          << psprecomp::hex32(runtime.memory().load32(address))
                          << " " << psprecomp::hex32(runtime.memory().load32(address + 4u))
                          << " " << psprecomp::hex32(runtime.memory().load32(address + 8u))
                          << " " << psprecomp::hex32(runtime.memory().load32(address + 12u))
                          << "\n";
            }
        }
        ctx.set_gpr(2, 0u);
        return;
    }
    auto view = runtime.memory().aot_fast_view();
    recomp_unit_0313_entry(runtime, ctx, 69u, view);
}

void asset_start_trace(psprecomp::Runtime &runtime,
                       psprecomp::AllegrexContext &ctx) {
    static int logged = 0;
    const std::uint32_t self = ctx.gpr[4];
    if (logged < 8 && runtime.memory().contains(self, 4u)) {
        const std::uint32_t vtable = runtime.memory().load32(self);
        const std::uint32_t hook =
            runtime.memory().contains(vtable + 180u, 4u)
                ? runtime.memory().load32(vtable + 180u) : 0u;
        std::cerr << "[tow2-asset] self=" << psprecomp::hex32(self)
                  << " vtable=" << psprecomp::hex32(vtable)
                  << " hook=" << psprecomp::hex32(hook)
                  << " a1=" << psprecomp::hex32(ctx.gpr[5])
                  << " a2=" << psprecomp::hex32(ctx.gpr[6])
                  << " a3=" << psprecomp::hex32(ctx.gpr[7])
                  << " a4=" << psprecomp::hex32(ctx.gpr[8]) << "\n";
        ++logged;
    }
    auto view = runtime.memory().aot_fast_view();
    recomp_unit_0301_entry(runtime, ctx, 372u, view);
}

void register_overrides(psprecomp::Runtime &runtime) {
    if (std::getenv("PSPRECOMP_TOW2_SKIP_INVALID_FREE") != nullptr) {
        runtime.register_function(0x08A7680Cu, &free_guard,
                                  "tow2_free_guard");
    }
    if (std::getenv("PSPRECOMP_TOW2_TRACE_ASSET_START") != nullptr) {
        runtime.register_function(0x08A5FA28u, &asset_start_trace,
                                  "tow2_asset_start_trace");
    }
}

void register_hle(psprecomp::Runtime &runtime, std::uint32_t user_arena_start) {
    g_mpeg_memory = &runtime.memory();
    if (std::getenv("PSPRECOMP_TOW2_SCENE_WATCH") != nullptr) {
        psprecomp::set_runtime_pre_dispatch_hook(&scene_caller_hook);
        psprecomp::set_runtime_post_dispatch_hook(&scene_watch_hook);
    }
    if (std::getenv("PSPRECOMP_TOW2_TITLE_TRIGGER") != nullptr) {
        runtime.register_function(kTitleCreatorThread,
                                  &title_creator_thread_entry,
                                  "tow2_title_creator_thread");
        runtime.register_function(kTitleStateThread,
                                  &title_state_thread_entry,
                                  "tow2_title_state_thread");
        runtime.register_function(kTitleThreadCreator,
                                  &title_thread_creator_entry,
                                  "tow2_title_thread_creator");
        runtime.register_function(kTitleState2Thread,
                                  &title_state2_thread_entry,
                                  "tow2_title_state2_thread");
        runtime.register_function(0x08A76ED8u, &ge_draw_stub,
                                  "tow2_ge_draw_stub");
    }
    g_partitions = PartitionTable{};
    g_partitions.next_address = (user_arena_start + 0xFFu) & ~0xFFu;
    g_module_end = user_arena_start;
    if (std::getenv("PSPRECOMP_TOW2_SCAN_POINTERS") != nullptr) {
        const std::array<std::uint32_t, 10> targets{
            0x088206D8u, 0x0896FEC4u, 0x08970A00u, 0x0881F5B0u, 0x08817298u,
            0x08813874u, 0x08814848u, 0x08817AD0u, 0x08814460u, 0x08818FB4u};
        for (std::uint32_t address = 0x08804000u;
             address + 4u <= user_arena_start; address += 4u) {
            if (!runtime.memory().contains(address, 4u)) continue;
            const std::uint32_t value = runtime.memory().load32(address);
            bool match = false;
            for (const std::uint32_t target : targets) {
                if (value == target) match = true;
            }
            if (!match) continue;
            std::cerr << "[tow2-ptr] " << psprecomp::hex32(value) << " at "
                      << psprecomp::hex32(address) << " words:";
            for (std::int32_t i = -4; i <= 4; ++i) {
                const std::uint32_t at = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(address) + i * 4);
                if (!runtime.memory().contains(at, 4u)) continue;
                std::cerr << " " << (i == 0 ? "*" : "")
                          << psprecomp::hex32(runtime.memory().load32(at));
            }
            std::cerr << "\n";
        }
        std::cerr << "[tow2-ptr] table 0x08C71E00..0x08C71F00:\n";
        for (std::uint32_t address = 0x08C71E00u; address < 0x08C71F00u;
             address += 0x10u) {
            std::cerr << "[tow2-ptr] " << psprecomp::hex32(address) << ":";
            for (std::uint32_t offset = 0; offset < 0x10u; offset += 4u) {
                std::cerr << " " << psprecomp::hex32(
                    runtime.memory().load32(address + offset));
            }
            std::cerr << "\n";
        }
    }
    load_libfile_module(runtime,
        runtime.game_root() / "PSP_GAME/USRDIR/module/libfile.prx");
    initialize_scheduler(runtime);

    runtime.register_hle("SysMemUserForUser", 0x7591C7DBu, // sceKernelSetCompiledSdkVersion
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("SysMemUserForUser", 0xEBD5C3E6u, // sceKernelSetCompiledSdkVersion395
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

    // Coherent host memory makes cache maintenance a no-op.
    for (const std::uint32_t nid : {0x79D1C3FAu, 0xB435DEC5u, 0xBFA98062u,
                                    0x34B9FA9Eu, 0x3EE30821u}) {
        runtime.register_hle("UtilsForUser", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2, 0u);
            });
    }

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
            const std::int32_t uid = g_partitions.next_uid++;
            g_partitions.blocks.emplace(uid, PartitionBlock{name, address, aligned_size});
            g_partitions.next_address = address + aligned_size;
            if (thread_diag() || partition_diag()) {
                std::cerr << "[tow2] partition alloc uid=" << uid << " name=\"" << name
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

    // Raw block allocator: Alloc returns a UID; GetMemoryBlockAddr writes the
    // guest address to the caller's pointer. The game uses this for its heap.
    runtime.register_hle("SysMemUserForUser", 0xFE707FDFu, // sceKernelAllocMemoryBlock
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t name_pointer = ctx.gpr[4];
            const std::uint32_t size = ctx.gpr[6];
            const std::uint32_t params = ctx.gpr[7];
            constexpr std::uint32_t alignment = 0x100u;
            if (params != 0u && rt.memory().contains(params, 4u) &&
                rt.memory().load32(params) != 4u) {
                ctx.set_gpr(2, 0x800200D3u);
                return;
            }
            const std::uint32_t aligned_size = (size + alignment - 1u) & ~(alignment - 1u);
            const std::uint32_t address =
                (g_partitions.next_address + alignment - 1u) & ~(alignment - 1u);
            if (aligned_size == 0u ||
                !rt.memory().contains(address, aligned_size) ||
                address + aligned_size > g_scheduler.next_stack_top) {
                ctx.set_gpr(2, 0x80020190u); // NO_MEMORY
                return;
            }
            std::string name = "memblock";
            if (name_pointer != 0u && rt.memory().contains(name_pointer, 1u)) {
                name = rt.memory().read_c_string(name_pointer, 128u);
            }
            rt.memory().zero(address, aligned_size);
            const std::int32_t uid = g_partitions.next_uid++;
            g_partitions.blocks.emplace(uid, PartitionBlock{name, address, aligned_size});
            g_partitions.next_address = address + aligned_size;
            if (thread_diag() || partition_diag()) {
                std::cerr << "[tow2] memblock alloc uid=" << uid << " name=\"" << name
                          << "\" addr=" << psprecomp::hex32(address)
                          << " size=" << psprecomp::hex32(aligned_size) << "\n";
            }
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("SysMemUserForUser", 0xDB83A952u, // sceKernelGetMemoryBlockAddr
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t out_pointer = ctx.gpr[5];
            const auto block = g_partitions.blocks.find(uid);
            if (block == g_partitions.blocks.end()) {
                ctx.set_gpr(2, 0x800200D3u);
                return;
            }
            if (out_pointer != 0u && rt.memory().contains(out_pointer, 4u)) {
                rt.memory().store32(out_pointer, block->second.address);
            }
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("SysMemUserForUser", 0x50F61D8Au, // sceKernelFreeMemoryBlock
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            ctx.set_gpr(2, g_partitions.blocks.erase(uid) == 1u ? 0u : 0x800200D3u);
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
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { sleep_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xD59EAD2Fu, // sceKernelWakeupThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wakeup_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x82826F70u, // sceKernelSleepThreadCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            // Spin only the title mount thread: it is created at the movie
            // handoff, so boot is unaffected. This keeps it runnable (priority
            // 17) and prevents the scheduler from resuming user_main (63)
            // before the mount and its asset dispatch have run.
            if (g_mount_thread_uid >= 0 &&
                g_scheduler.current_uid == g_mount_thread_uid &&
                std::getenv("PSPRECOMP_TOW2_MOUNT_SPIN") != nullptr) {
                ctx.set_gpr(2, 0u);
                return;
            }
            if (std::getenv("PSPRECOMP_TOW2_SLEEP_CB_NOW") != nullptr) {
                delay_current_thread(rt, ctx, 1000u);
                return;
            }
            sleep_thread(rt, ctx);
        });

    runtime.register_hle("ThreadManForUser", 0x8125221Du, // sceKernelCreateMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_mbx(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xE9B3061Eu, // sceKernelSendMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { send_mbx(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x18260574u, // sceKernelReceiveMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { receive_mbx(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x0D81716Au, // sceKernelPollMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { poll_mbx(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x86255ADAu, // sceKernelDeleteMbx
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_mbx(rt, ctx); });

    runtime.register_hle("ThreadManForUser", 0x55C20A00u, // sceKernelCreateEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_event_flag(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x1FB15A32u, // sceKernelSetEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { set_event_flag(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x812346E4u, // sceKernelClearEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { clear_event_flag(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x402FCF22u, // sceKernelWaitEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_event_flag(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x30FD48F0u, // sceKernelPollEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { poll_event_flag(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xA66B0120u, // sceKernelReferEventFlagStatus
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { refer_event_flag_status(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xEF9E4C70u, // sceKernelDeleteEventFlag
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_event_flag(rt, ctx); });

    runtime.register_hle("ThreadManForUser", 0xB7D098C6u, // sceKernelCreateMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_mutex(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xB011B11Fu, // sceKernelLockMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { lock_mutex(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x6B30100Fu, // sceKernelUnlockMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { unlock_mutex(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xF8170FBEu, // sceKernelDeleteMutex
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_mutex(rt, ctx); });

    runtime.register_hle("ThreadManForUser", 0xD6DA4BA1u, // sceKernelCreateSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_semaphore(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x4E3A1105u, // sceKernelWaitSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_semaphore(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x6D212BACu, // sceKernelWaitSemaCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_semaphore(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x3F53E640u, // sceKernelSignalSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { signal_semaphore(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x58B1F937u, // sceKernelPollSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { poll_semaphore(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x28B6489Cu, // sceKernelDeleteSema
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_semaphore(rt, ctx); });

    runtime.register_hle("ThreadManForUser", 0xE81CAF8Fu, // sceKernelCreateCallback
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_callback(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xEDBA5844u, // sceKernelDeleteCallback
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_callback(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x2A3D44FFu, // sceKernelGetCallbackCount
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("ThreadManForUser", 0x349D6D6Cu, // sceKernelCheckCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("ThreadManForUser", 0x446D8DE6u, // sceKernelCreateThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xF475845Du, // sceKernelStartThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { start_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xAA73C935u, // sceKernelExitThread
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (auto current = g_scheduler.threads.find(g_scheduler.current_uid);
                current != g_scheduler.threads.end()) {
                current->second.exit_status = ctx.gpr[4];
            }
            complete_current_thread(rt, ctx);
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
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_thread_end(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x840E8133u, // sceKernelWaitThreadEndCB
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_thread_end(rt, ctx); });
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

    runtime.register_hle("sceDmac", 0x617F3FE6u, // sceDmacMemcpy
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t destination = ctx.gpr[4];
            const std::uint32_t source = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            if (mpeg_diag()) {
                std::cerr << "[tow2-mpeg] dma dst=" << psprecomp::hex32(destination)
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
            // The movie path writes 512x272 ABGR8888 frames into VRAM before
            // the GE blits them to the 16-bit display buffer. Until the GE
            // interpreter is ported, present those movie buffers directly so
            // the decoded opening movie is visible.
            if ((destination == 0x04088000u || destination == 0x04110000u) &&
                size == 557056u) {
                window_present(rt.memory(), destination, 512u, 3u);
            }
            ctx.set_gpr(2, 0u);
        });

    // Minimal MPEG lifecycle contract. The title opens the opening-movie
    // stream during startup; decoding itself is not part of this profile yet.
    runtime.register_hle("sceMpeg", 0x682A619Bu, // sceMpegInit
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (mpeg_diag()) std::cerr << "[tow2-mpeg] init\n";
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
                std::cerr << "[tow2-mpeg] ringbuffer-construct ring=" << psprecomp::hex32(ringbuffer)
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
                std::cerr << "[tow2-mpeg] create mpeg=" << psprecomp::hex32(mpeg_address)
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
                std::cerr << "[tow2-mpeg] query-offset mpeg=" << psprecomp::hex32(ctx.gpr[4])
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
                std::cerr << "[tow2-mpeg] regist mpeg=" << psprecomp::hex32(ctx.gpr[4])
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
                std::cerr << "[tow2-mpeg] ringbuffer-put ring=" << psprecomp::hex32(ringbuffer)
                          << " num=" << ctx.gpr[5] << " avail=" << ctx.gpr[6] << "\n";
            }
            const std::uint32_t packets = rt.memory().load32(ringbuffer + 0u);
            const std::uint32_t written = rt.memory().load32(ringbuffer + 8u);
            const std::uint32_t used = rt.memory().load32(ringbuffer + 12u);
            const std::uint32_t requested = ctx.gpr[5];
            std::uint32_t num = requested;
            // The PMF's first 2048-byte packet is the PSMF header; the movie
            // worker's ring-buffer accounting must stop at the remaining data
            // packets or its byte counter overshoots the stream target and the
            // movie-player state machine never reaches its exit state.
            if (g_pmf_packet_count > 1u) {
                const std::uint32_t cap = g_pmf_packet_count - 1u;
                const std::uint32_t remaining = written < cap ? cap - written : 0u;
                num = std::min(num, remaining);
            }
            rt.memory().store32(ringbuffer + 8u, written + num);
            rt.memory().store32(ringbuffer + 12u, std::min(packets, used + num));
            ctx.set_gpr(2, num);
        });
    runtime.register_hle("sceMpeg", 0x800C44DFu, // sceMpegAtracDecode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t init = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow2-mpeg] atrac-decode mpeg=" << psprecomp::hex32(ctx.gpr[4])
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
                            std::cerr << "[tow2-mpeg] atrac-pcm bytes="
                                      << decoded_bytes << " frames="
                                      << g_atrac_player->decoded_frames() << "\n";
                        }
                    }
                    rt.memory().copy_in(audio_buffer,
                        std::span<const std::uint8_t>(pcm.data(), pcm.size()));
                } else {
                    if (mpeg_diag()) std::cerr << "[tow2-mpeg] " << error << "\n";
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
                std::cerr << "[tow2-mpeg] get-avc mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " stream=" << psprecomp::hex32(ctx.gpr[5])
                          << " au=" << psprecomp::hex32(au) << " attr=" << psprecomp::hex32(attr) << "\n";
            }
            if (au != 0u && rt.memory().contains(au, 0x18u)) {
                rt.memory().zero(au, 0x18u);
                rt.memory().store32(au + 0x14u, 2048u);
            }
            if (attr != 0u && rt.memory().contains(attr, 4u))
                rt.memory().store32(attr, 0u);
            if (g_mpeg_video_eof) {
                ctx.set_gpr(2, 0x80618001u); // SCE_MPEG_ERROR_NO_DATA
                return;
            }
            consume_mpeg_packet(rt, ctx.gpr[4]);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0xE1CE83A7u, // sceMpegGetAtracAu
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t au = ctx.gpr[6];
            const std::uint32_t attr = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow2-mpeg] get-atrac mpeg=" << psprecomp::hex32(ctx.gpr[4])
                          << " stream=" << psprecomp::hex32(ctx.gpr[5])
                          << " au=" << psprecomp::hex32(au) << "\n";
            }
            if (au != 0u && rt.memory().contains(au, 0x18u)) {
                rt.memory().zero(au, 0x18u);
                rt.memory().store32(au + 0x14u, 0u);
            }
            if (attr != 0u && rt.memory().contains(attr, 4u))
                rt.memory().store32(attr, 0u);
            if (g_mpeg_video_eof) {
                ctx.set_gpr(2, 0x80618001u); // SCE_MPEG_ERROR_NO_DATA
                return;
            }
            consume_mpeg_packet(rt, ctx.gpr[4]);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceMpeg", 0x0E3C2E9Du, // sceMpegAvcDecode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t init = ctx.gpr[8];
            const std::uint32_t buffer_ptr = ctx.gpr[7];
            if (mpeg_diag()) {
                std::cerr << "[tow2-mpeg] avc-decode mpeg=" << psprecomp::hex32(ctx.gpr[4])
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
                if (mpeg_diag()) std::cerr << "[tow2-mpeg] " << error << "\n";
                if (init != 0u && rt.memory().contains(init, 4u))
                    rt.memory().store32(init, 0u);
                ctx.set_gpr(2, 1u);
                return;
            }
            int decoded_width = 0;
            int decoded_height = 0;
            const bool decoded = g_mpeg_player->decode_next(
                g_mpeg_frame, decoded_width, decoded_height);
            if (!decoded) {
                g_mpeg_video_eof = true;
                if (mpeg_diag()) {
                    std::cerr << "[tow2-mpeg] end of video stream\n";
                    if (MpegState *eof_state = mpeg_state_for(rt, ctx.gpr[4]);
                        eof_state != nullptr && eof_state->ringbuffer != 0u &&
                        rt.memory().contains(eof_state->ringbuffer, 48u)) {
                        std::cerr << "[tow2-mpeg] eof ring packets="
                                  << rt.memory().load32(eof_state->ringbuffer + 0u)
                                  << " read=" << rt.memory().load32(eof_state->ringbuffer + 4u)
                                  << " written=" << rt.memory().load32(eof_state->ringbuffer + 8u)
                                  << " used=" << rt.memory().load32(eof_state->ringbuffer + 12u)
                                  << "\n";
                    }
                }
                if (init != 0u && rt.memory().contains(init, 4u))
                    rt.memory().store32(init, 0u);
                // Clear the stale "stream active" bit and publish the
                // completion event once so the file reader can drain.
                reset_named_event_flag(rt, "cOnMediaStream", 0x0Fu);
                // PPSSPP A/B: the real movie task ends with [fp+0x2C] == 0.
                // Treat end-of-stream as a successful no-frame decode.
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
                    std::cerr << "[tow2-mpeg] write failed dest="
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
                std::cerr << "[tow2-mpeg] decode-mode mpeg=" << psprecomp::hex32(ctx.gpr[4])
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
    // ---------------------------------------------------------------------
    // Boot-level GE/display contract.
    //
    // Lists are accepted and reported complete, but no command stream is
    // rasterized yet. Replace the enqueue stubs with the tow-2 GE interpreter
    // in the rendering phase; do not mistake this for working graphics.
    // ---------------------------------------------------------------------
    runtime.register_hle("sceGe_user", 0xE47E40E4u, // sceGeEdramGetAddr
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0x04000000u);
        });
    runtime.register_hle("sceGe_user", 0x1F6752ADu, // sceGeEdramGetSize
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0x00200000u);
        });
    runtime.register_hle("sceGe_user", 0xAB49E76Au, // sceGeListEnQueue
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    runtime.register_hle("sceGe_user", 0x1C0D95A6u, // sceGeListEnQueueHead
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
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
            window_present(rt.memory(), g_framebuffer, g_framebuffer_stride,
                           g_framebuffer_format);
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x36CDFADEu, // sceDisplayWaitVblankStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_scheduler.now_us += 16683u;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x46F186C3u, // sceDisplayWaitVblankStartCB
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_scheduler.now_us += 16683u;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x8EB9EC49u, // sceDisplayWaitVblankCB
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_scheduler.now_us += 16683u;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x984C27E7u, // sceDisplayWaitVblank
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_scheduler.now_us += 16683u;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceDisplay", 0x9C6EAAD7u, // sceDisplayGetVcount
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t vcount =
                static_cast<std::uint32_t>(g_scheduler.now_us / 16683u);
            if ((std::getenv("PSPRECOMP_TOW2_MOVIE_STOP") != nullptr ||
                 std::getenv("PSPRECOMP_TOW2_MOVIE_COUNTERS") != nullptr) &&
                ctx.gpr[31] >= 0x08A63C00u && ctx.gpr[31] < 0x08A63D00u) {
                g_movie_vcount = vcount;
                g_movie_fp = ctx.gpr[30];
                if (g_movie_fp != 0u && rt.memory().contains(g_movie_fp, 0x38u)) {
                    g_movie_counter_18 =
                        rt.memory().load32(g_movie_fp + 0x18u);
                    g_movie_counter_1c =
                        rt.memory().load32(g_movie_fp + 0x1Cu);
                    g_movie_counter_30 =
                        rt.memory().load32(g_movie_fp + 0x30u);
                    g_movie_counter_34 =
                        rt.memory().load32(g_movie_fp + 0x34u);
                }
            }
            if (std::getenv("PSPRECOMP_TOW2_MOVIE_COUNTERS") != nullptr &&
                ctx.gpr[31] >= 0x08A63C00u && ctx.gpr[31] < 0x08A63D00u) {
                static std::uint32_t last_vcount = 0xFFFFFFFFu;
                static std::uint32_t logs = 0u;
                if (vcount != last_vcount && logs < 240u) {
                    const std::uint32_t fp = ctx.gpr[30];
                    if (fp != 0u && rt.memory().contains(fp, 0x34u)) {
                        std::cerr << "[movie-counters] ra="
                                  << psprecomp::hex32(ctx.gpr[31])
                                  << " fp=" << psprecomp::hex32(fp)
                                  << " +18=" << psprecomp::hex32(
                                         rt.memory().load32(fp + 0x18u))
                                  << " +1C=" << psprecomp::hex32(
                                         rt.memory().load32(fp + 0x1Cu))
                                  << " +30=" << psprecomp::hex32(
                                         rt.memory().load32(fp + 0x30u))
                                  << " vcount=" << vcount << "\n";
                        last_vcount = vcount;
                        ++logs;
                    }
                }
            }
            ctx.set_gpr(2, vcount);
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
            if (std::getenv("PSPRECOMP_TOW2_PRESS_START") != nullptr) {
                // Press Start+Cross for 250 ms every 2 s of guest time.
                if ((g_scheduler.now_us % 2000000u) < 250000u) buttons |= 0x4008u;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t at = buffer + index * 16u;
                rt.memory().zero(at, 16u);
                rt.memory().store32(at, static_cast<std::uint32_t>(g_scheduler.now_us));
                rt.memory().store32(at + 4u, buttons);
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
    runtime.register_hle("sceUtility", 0x50C4CD57u, // sceUtilitySavedataInitStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            // The loader treats SHUTDOWN (3) as the completed-dialog state.
            g_savedata_status = 3;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUtility", 0x8874DBE0u, // sceUtilitySavedataGetStatus
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_savedata_status));
        });
    runtime.register_hle("sceUtility", 0xD4B95FFBu, // sceUtilitySavedataUpdate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_savedata_status = 0;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceUtility", 0x9790B33Cu, // sceUtilitySavedataShutdownStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_savedata_status = 0;
            ctx.set_gpr(2, 0u);
        });

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
    runtime.register_hle("sceAudio", 0x136CAF51u, audio_block); // OutputBlocking
    runtime.register_hle("sceAudio", 0x13F592BCu, audio_block); // OutputPannedBlocking
    runtime.register_hle("sceAudio", 0xE2D56B2Du, // OutputPanned (non-blocking)
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    for (const std::uint32_t nid : {0x95FD0C2Du, 0xB7E1D8E7u, 0xCB2E439Eu,
                                    0xB011922Fu}) {
        runtime.register_hle("sceAudio", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2, 0u);
            });
    }

    runtime.register_hle("sceSasCore", 0x42778A9Fu, // __sceSasInit
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_sas.core = ctx.gpr[4];
            g_sas.grain = static_cast<std::int32_t>(ctx.gpr[5]);
            g_sas.max_voices = static_cast<std::int32_t>(ctx.gpr[6]);
            g_sas.output_mode = static_cast<std::int32_t>(ctx.gpr[7]);
            g_sas.sample_rate = static_cast<std::int32_t>(ctx.gpr[8]);
            ctx.set_gpr(2, 0u);
        });
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
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, g_sas.end_flag);
        });
    runtime.register_hle("sceSasCore", 0x2C8E6AB3u, // __sceSasGetPauseFlag
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceSasCore", 0x74AE582Au, // __sceSasGetEnvelopeHeight
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceSasCore", 0x07F58C24u, // __sceSasGetAllEnvelopeHeights
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t pointer = ctx.gpr[5];
            constexpr std::uint32_t kVoicesMax = 32u;
            if (pointer != 0u && rt.memory().contains(pointer, kVoicesMax * 4u)) {
                rt.memory().zero(pointer, kVoicesMax * 4u);
            }
            ctx.set_gpr(2, 0u);
        });
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
    // Override the generic setters with the key-on/off completion contract.
    runtime.register_hle("sceSasCore", 0x76F01ACAu, // __sceSasSetKeyOn
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_sas.end_flag = 0u;
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("sceSasCore", 0xA0CF2FA4u, // __sceSasSetKeyOff
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("sceOpenPSID", 0xC69BEBCEu, // sceOpenPSIDGetOpenPSID
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t pointer = ctx.gpr[4];
            if (pointer != 0u && rt.memory().contains(pointer, 16u)) {
                rt.memory().zero(pointer, 16u);
            }
            ctx.set_gpr(2, 0u);
        });

    runtime.register_hle("libfile", 0xD8E69C7Du, // tow-2 libfile export
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!g_libfile_loaded) {
                ctx.set_gpr(2, 0xFFFFFFFFu);
                return;
            }
            // Call the real libfile export in an isolated context. The module
            // is loaded at kLibfileBase; its export is at base+0x10.
            psprecomp::AllegrexContext module_ctx = ctx;
            module_ctx.pc = kLibfileBase + 0x10u;
            module_ctx.set_gpr(28, g_libfile_gp);
            const std::uint32_t stack = g_libfile_stack_top - 0x100u;
            module_ctx.set_gpr(26, stack);
            module_ctx.set_gpr(29, stack);
            module_ctx.set_gpr(30, stack);
            module_ctx.set_gpr(31, kLibfileBase); // sentinel return
            if (!rt.invoke_isolated_aot(module_ctx.pc, module_ctx)) {
                ctx.set_gpr(2, 0xFFFFFFFFu);
                return;
            }
            ctx.set_gpr(2, module_ctx.gpr[2]);
            ctx.set_gpr(3, module_ctx.gpr[3]);
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
    runtime.register_hle("sceLibFont", 0x3AEA8CB6u, // sceFontClose
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_close(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x574B6FBCu, // sceFontDoneLib
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_done(rt, ctx); });
    runtime.register_hle("sceLibFont", 0xEE232411u, // sceFontSetAltCharacterCode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { font_set_alt(rt, ctx); });
    runtime.register_hle("sceLibFont", 0x02D7F94Bu, // sceFontFlush
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0x5C3E4A9Eu, // sceFontGetCharImageRect
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            font_get_char_image_rect(rt, ctx);
        });

    runtime.register_hle("scePower", 0x04B7766Eu, // scePowerRegisterCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("scePower", 0xDFA8BAF8u, // scePowerUnregisterCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });

    // The disc is modelled as permanently present/ready/readable.
    runtime.register_hle("sceUmdUser", 0xAEE7404Du, // sceUmdRegisterUMDCallBack
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceUmdUser", 0x46EBB729u, // sceUmdCheckMedium
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    runtime.register_hle("sceUmdUser", 0x6B4A146Cu, // sceUmdGetDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0x32u); });
    runtime.register_hle("sceUmdUser", 0x8EF08FCEu, // sceUmdWaitDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceUmdUser", 0x4A9E5E29u, // sceUmdWaitDriveStatCB
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceUmdUser", 0x6AF9B50Au, // sceUmdWaitDriveStatWithTimer
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceUmdUser", 0xC6183D47u, // sceUmdActivate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    runtime.register_hle("LoadExecForUser", 0x4AC57943u, // sceKernelRegisterExitCallback
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("LoadExecForUser", 0x05572A5Fu, // sceKernelExitGame
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &) {
            rt.stop("sceKernelExitGame");
        });

    runtime.register_hle("ModuleMgrForUser", 0xF0A26395u, // sceKernelGetModuleId
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 1u);
        });
    runtime.register_hle("ModuleMgrForUser", 0xD8B73127u, // sceKernelGetModuleIdByAddress
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            const bool in_main_module =
                address >= psprecomp::kDefaultPspUserLoadBase && address < g_module_end;
            ctx.set_gpr(2, in_main_module ? 1u : 0x80020130u);
        });

    const std::uint64_t interval = timeslice_interval();
    if (interval != 0u) {
        psprecomp::set_runtime_starvation_hook(&starvation_preempt, interval);
    }

    std::cout << "[tow2] scheduler and kernel objects initialized"
              << " (timeslice=" << interval << ")\n";
}

} // namespace tow2
