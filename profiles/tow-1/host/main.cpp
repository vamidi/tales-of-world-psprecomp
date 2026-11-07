#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "tow1_hle.h"
#include "tow1_ge.hpp"
#include "tow1_window.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr std::uint64_t kDefaultMaxDispatches = 10'000'000ull;

std::uint64_t parse_dispatches(const char *text) {
    if (text == nullptr || *text == '\0') return kDefaultMaxDispatches;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0u) {
        throw psprecomp::Error(std::string("Invalid dispatch cap: ") + text);
    }
    return static_cast<std::uint64_t>(parsed);
}

} // namespace

int main(int argc, char **argv) {
    try {
        // Usage: Tow1Native [eboot] [game_root] [max_dispatches]
        const std::filesystem::path executable = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("profiles/tow-1/game/eboot.dec");
        const std::filesystem::path game_root = argc > 2
            ? std::filesystem::path(argv[2])
            : std::filesystem::path("profiles/tow-1/game/root");
        const std::uint64_t max_dispatches = argc > 3
            ? parse_dispatches(argv[3])
            : parse_dispatches(std::getenv("PSPRECOMP_MAX_DISPATCHES"));

        if (!std::filesystem::is_regular_file(executable)) {
            throw psprecomp::Error("EBOOT not found: " + executable.string());
        }

        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(executable);
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        runtime.set_game_root(game_root);

        const auto relocations = elf.load_and_relocate(
            runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        if (relocations.unsupported != 0u || relocations.invalid != 0u) {
            throw psprecomp::Error(
                "Relocation gate failed: " + std::to_string(relocations.unsupported) +
                " unsupported, " + std::to_string(relocations.invalid) + " invalid");
        }

        const auto module = elf.find_module_info(
            runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        if (!module) {
            throw psprecomp::Error("PSP module info not found after relocation");
        }
        runtime.cpu().set_gpr(28, module->gp);

        std::uint64_t image_end = 0u;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue; // PT_LOAD
            const std::uint64_t start = elf.segment_runtime_address(
                index, psprecomp::kDefaultPspUserLoadBase);
            image_end = std::max(image_end, start + segment.memory_size);
        }
        if (image_end == 0u || image_end > 0x0A000000ull) {
            throw psprecomp::Error("Invalid PSP ELF load image extent");
        }
        const std::uint32_t user_arena_start =
            static_cast<std::uint32_t>((image_end + 0xFFu) & ~0xFFull);

        // Reserve the loader/module_start stack at the top of PSP user RAM.
        // Without this the first generated prologue subtracts from sp=0 and
        // faults before any guest code can run. This must happen before the
        // scheduler records the module thread.
        constexpr std::uint32_t kUserRamTop = 0x0A000000u;
        constexpr std::uint32_t kModuleStackSize = 0x10000u;
        const std::uint32_t stack_bottom = kUserRamTop - kModuleStackSize;
        if (!runtime.memory().contains(stack_bottom, kModuleStackSize)) {
            throw psprecomp::Error("Module stack falls outside PSP user RAM");
        }
        runtime.memory().zero(stack_bottom, kModuleStackSize);
        const std::uint32_t kernel_context = kUserRamTop - 0x100u;
        runtime.cpu().set_gpr(26, kernel_context); // k0
        runtime.cpu().set_gpr(29, kernel_context); // sp
        runtime.cpu().set_gpr(30, kernel_context); // fp

        tow1::register_hle(runtime, user_arena_start);
        psprecomp::register_generated_functions(runtime);

        if (runtime.function_count() == 0u) {
            throw psprecomp::Error("No generated functions registered");
        }

        // Entry-state convention used by the working profile: no caller frame,
        // no arguments. module_start takes it from there.
        runtime.cpu().set_gpr(31, 0u);
        runtime.cpu().set_gpr(4, 0u);
        runtime.cpu().set_gpr(5, 0u);

        std::cout << "Tow1Native PSP bootstrap\n"
                  << "Executable: " << executable.string() << "\n"
                  << "Game root:  " << runtime.game_root().string()
                  << (std::filesystem::exists(runtime.game_root()) ? "" : " (missing)") << "\n"
                  << "Entry:      " << psprecomp::hex32(elf.runtime_entry()) << "\n"
                  << "Module:     " << module->name << "\n"
                  << "gp:         " << psprecomp::hex32(module->gp) << "\n"
                  << "Relocs:     " << relocations.total << " (invalid "
                  << relocations.invalid << ", unsupported "
                  << relocations.unsupported << ")\n"
                  << "Functions:  " << runtime.function_count() << "\n"
                  << "Dispatch cap: " << max_dispatches << "\n";

        tow1::window_start();
        runtime.run(elf.runtime_entry(), max_dispatches);
        tow1::ge_shutdown();

        psprecomp::report_counted_pcs();
        if (std::getenv("PSPRECOMP_TOW1_TRANSITION_DUMP") != nullptr) {
            const auto &memory = runtime.memory();
            const auto dump = [&memory](std::uint32_t from, std::uint32_t to) {
                for (std::uint32_t at = from; at < to; at += 16u) {
                    std::cerr << "[transition] " << psprecomp::hex32(at) << ":";
                    for (std::uint32_t offset = 0; offset < 16u; offset += 4u)
                        std::cerr << " " << psprecomp::hex32(memory.load32(at + offset));
                    std::cerr << "\n";
                }
            };
            dump(0x08D42224u, 0x08D422A4u);
            const std::uint32_t queue = memory.load32(0x08D42224u);
            std::cerr << "[transition] queue=" << psprecomp::hex32(queue) << "\n";
            if (memory.contains(queue, 0x80u))
                dump(queue, queue + 0x80u);
            std::cerr << "[transition] title GE list:\n";
            dump(0x08E0F740u, 0x08E0F940u);
            std::cerr << "[transition] title texture:\n";
            dump(0x08FD45C0u, 0x08FD4640u);
            std::cerr << "[transition] ppt texture object buffer:\n";
            dump(0x08EA0700u, 0x08EA0780u);
            std::cerr << "[transition] title sprite object:\n";
            dump(0x08E97600u, 0x08E97700u);
            dump(0x08E98160u, 0x08E98260u);
            dump(0x09FEF900u, 0x09FEFA00u);
            dump(0x08BAE750u, 0x08BAE7D0u);
            std::cerr << "[transition] title clut:\n";
            dump(0x08FF4400u, 0x08FF4480u);
            std::cerr << "[transition] gzip title.arc candidates:";
            for (std::uint32_t at = 0x08000000u; at < 0x0A000000u; at += 4u) {
                if (memory.load32(at) == 0x08088B1Fu)
                    std::cerr << " " << psprecomp::hex32(at);
            }
            std::cerr << "\n[transition] title archive buffer:\n";
            dump(0x09C60000u, 0x09C60400u);
            std::cerr << "[transition] BNG filename candidates:";
            for (std::uint32_t at = 0x08000000u; at < 0x0A000000u; at += 4u) {
                if (memory.load32(at) == 0x5F474E42u &&
                    memory.load32(at + 4u) == 0x5F726F66u &&
                    memory.load32(at + 8u) == 0x2E707370u &&
                    memory.load32(at + 12u) == 0x00747070u)
                    std::cerr << " " << psprecomp::hex32(at);
            }
            std::cerr << "\n";
        }
        tow1::report_threads();
        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";
        runtime.report_hle_histogram();
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &e) {
        std::cerr << "Tow1Native error: " << e.what() << "\n";
        tow1::report_threads();
        return 1;
    }
}
