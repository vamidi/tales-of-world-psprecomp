#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "tow2_hle.h"
#include "tow2_window.hpp"

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
        // Usage: Tow2Native [eboot] [game_root] [max_dispatches]
        const std::filesystem::path executable = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("profiles/tow-2/game/eboot.dec");
        const std::filesystem::path game_root = argc > 2
            ? std::filesystem::path(argv[2])
            : std::filesystem::path("profiles/tow-2/game/root");
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

        if (std::getenv("PSPRECOMP_TOW2_SCAN_TITLE_PTR") != nullptr) {
            constexpr std::uint32_t kTitleArcString = 0x08C239B0u;
            constexpr std::uint32_t kTitlePmfString = 0x08C239A6u;
            constexpr std::uint32_t kTitleTableString = 0x08C23960u;
            for (std::uint32_t address = 0x08800000u; address < 0x0A000000u;
                 address += 4u) {
                if (!runtime.memory().contains(address, 4u)) continue;
                const std::uint32_t value = runtime.memory().load32(address);
                if (value == kTitleArcString || value == kTitlePmfString ||
                    value == kTitleTableString) {
                    std::cerr << "[tow2-scan] ptr=" << psprecomp::hex32(value)
                              << " at=" << psprecomp::hex32(address) << "\n";
                }
            }
            constexpr std::uint32_t kStateSlots[] = {
                0x08D14F2Cu, 0x08D14F54u, 0x08D14F10u, 0x08D03100u};
            for (const std::uint32_t slot : kStateSlots) {
                if (!runtime.memory().contains(slot, 4u)) continue;
                const std::uint32_t value = runtime.memory().load32(slot);
                std::cerr << "[tow2-state] [" << psprecomp::hex32(slot)
                          << "]=" << psprecomp::hex32(value);
                if (value != 0u && runtime.memory().contains(value, 8u)) {
                    std::cerr << " +0=" << psprecomp::hex32(runtime.memory().load32(value))
                              << " +4=" << psprecomp::hex32(runtime.memory().load32(value + 4u));
                }
                std::cerr << "\n";
            }
        }

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

        tow2::register_hle(runtime, user_arena_start);
        psprecomp::register_generated_functions(runtime);

        if (runtime.function_count() == 0u) {
            throw psprecomp::Error("No generated functions registered");
        }

        // Entry-state convention used by the working profile: no caller frame,
        // no arguments. module_start takes it from there.
        runtime.cpu().set_gpr(31, 0u);
        runtime.cpu().set_gpr(4, 0u);
        runtime.cpu().set_gpr(5, 0u);

        std::cout << "Tow2Native PSP bootstrap\n"
                  << "Executable: " << executable.string() << "\n"
                  << "Game root:  " << runtime.game_root().string()
                  << (std::filesystem::exists(runtime.game_root()) ? "" : " (missing)") << "\n"
                  << "Entry:      " << psprecomp::hex32(elf.runtime_entry()) << "\n"
                  << "Module:     " << module->name << "\n"
                  << "gp:         " << psprecomp::hex32(module->gp) << "\n"
                  << "Arena:      " << psprecomp::hex32(user_arena_start) << "\n"
                  << "Relocs:     " << relocations.total << " (invalid "
                  << relocations.invalid << ", unsupported "
                  << relocations.unsupported << ")\n"
                  << "Functions:  " << runtime.function_count() << "\n"
                  << "Dispatch cap: " << max_dispatches << "\n";

        tow2::window_start();
        runtime.run(elf.runtime_entry(), max_dispatches);
        tow2::window_shutdown();
        tow2::report_threads();
        psprecomp::report_counted_pcs();
        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";
        runtime.report_hle_histogram();
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &e) {
        std::cerr << "Tow2Native error: " << e.what() << "\n";
        return 1;
    }
}
