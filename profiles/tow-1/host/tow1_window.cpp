#include "tow1_window.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tow1 {
namespace {

constexpr std::uint32_t kWidth = 480u;
constexpr std::uint32_t kHeight = 272u;
constexpr std::uint32_t kPresentMessage = WM_APP + 1u;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_close_requested{false};
std::atomic<std::uint32_t> g_buttons{0u};
std::mutex g_mutex;
std::condition_variable g_ready;
std::vector<std::uint32_t> g_frame;
bool g_have_frame = false;
std::atomic<bool> g_post_osk_capture_armed{false};
std::string g_post_osk_capture_path;
std::uint32_t g_post_osk_capture_delay = 0u;
std::uint32_t g_post_osk_capture_base = 0u;
std::uint32_t g_post_osk_range_start = 0u;
std::uint32_t g_post_osk_range_end = 0u;
std::uint32_t g_post_osk_range_step = 0u;
std::uint32_t g_present_count = 0u;
std::uint32_t g_post_osk_present_base = 0u;
struct ScheduledInput {
    std::uint32_t start{};
    std::uint32_t end{};
    std::uint32_t mask{};
};
std::vector<ScheduledInput> g_scheduled_inputs;

#if defined(_WIN32)
HWND g_hwnd = nullptr;
std::thread g_thread;
unsigned g_scale = 2u;

std::uint32_t load_pixel(const psprecomp::GuestMemory &memory, std::uint32_t address,
                         std::uint32_t format) {
    switch (format) {
    case 3u: {
        const std::uint32_t p = memory.load32(address);
        // PSP 8888 framebuffer is stored as A B G R in memory.
        return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16u) | ((p & 0x000000FFu) << 16u);
    }
    case 0u: {
        const std::uint32_t p = memory.load16(address);
        const std::uint32_t r = (p >> 11u) & 0x1Fu;
        const std::uint32_t g = (p >> 5u) & 0x3Fu;
        const std::uint32_t b = p & 0x1Fu;
        return 0xFF000000u | ((r * 255u / 31u) << 16u) |
               ((g * 255u / 63u) << 8u) | (b * 255u / 31u);
    }
    case 1u: {
        const std::uint32_t p = memory.load16(address);
        const std::uint32_t r = (p >> 10u) & 0x1Fu;
        const std::uint32_t g = (p >> 5u) & 0x1Fu;
        const std::uint32_t b = p & 0x1Fu;
        return 0xFF000000u | ((r * 255u / 31u) << 16u) |
               ((g * 255u / 31u) << 8u) | (b * 255u / 31u);
    }
    default: {
        const std::uint32_t p = memory.load16(address);
        const std::uint32_t r = (p >> 8u) & 0xFu;
        const std::uint32_t g = (p >> 4u) & 0xFu;
        const std::uint32_t b = p & 0xFu;
        return 0xFF000000u | ((r * 17u) << 16u) | ((g * 17u) << 8u) | (b * 17u);
    }
    }
}

void paint(HWND hwnd) {
    PAINTSTRUCT paint_struct{};
    HDC dc = BeginPaint(hwnd, &paint_struct);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    std::vector<std::uint32_t> pixels;
    {
        std::lock_guard lock(g_mutex);
        if (g_have_frame) pixels = g_frame;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(kWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(kHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (pixels.empty()) {
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
    } else {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, width, height, 0, 0,
                      static_cast<int>(kWidth), static_cast<int>(kHeight),
                      pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(hwnd, &paint_struct);
}

std::uint32_t key_to_button(WPARAM key) {
    switch (key) {
    case VK_UP: return 0x0010u;
    case VK_RIGHT: return 0x0020u;
    case VK_DOWN: return 0x0040u;
    case VK_LEFT: return 0x0080u;
    case VK_RETURN: return 0x0008u; // Start
    case VK_BACK: return 0x0001u;   // Select
    case 'Z': return 0x4000u;       // Cross
    case 'X': return 0x2000u;       // Circle
    case 'A': return 0x1000u;       // Triangle
    case 'S': return 0x8000u;       // Square
    case 'Q': return 0x0100u;       // L trigger
    case 'W': return 0x0200u;       // R trigger
    default: return 0u;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        g_buttons.fetch_or(key_to_button(wparam));
        return 0;
    case WM_KEYUP:
        g_buttons.fetch_and(~key_to_button(wparam));
        return 0;
    case WM_SETFOCUS:
        g_buttons.store(0u);
        return 0;
    case WM_KILLFOCUS:
        g_buttons.store(0u);
        return 0;
    case kPresentMessage:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        g_close_requested.store(true);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, message, wparam, lparam);
    }
}

void window_thread() {
    const HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t *class_name = L"Tow1NativeDisplay";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = class_name;
    RegisterClassExW(&window_class);

    RECT rect{0, 0, static_cast<LONG>(kWidth * g_scale),
              static_cast<LONG>(kHeight * g_scale)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExW(0, class_name, L"Tales of the World: Radiant Mythology",
                             WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             nullptr, nullptr, instance, nullptr);
    {
        std::lock_guard lock(g_mutex);
        g_ready.notify_all();
    }
    if (g_hwnd == nullptr) return;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG message{};
    while (!g_close_requested.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

#else

void window_thread() {}
std::uint32_t load_pixel(const psprecomp::GuestMemory &, std::uint32_t, std::uint32_t) {
    return 0u;
}

#endif

} // namespace

void window_start() {
    if (const char *disabled = std::getenv("PSPRECOMP_TOW1_NO_WINDOW");
        disabled != nullptr && *disabled != '\0') {
        g_enabled.store(false);
        return;
    }
    if (const char *scale = std::getenv("PSPRECOMP_TOW1_SCALE"); scale != nullptr) {
        const int value = std::atoi(scale);
        if (value >= 1 && value <= 8) {
#if defined(_WIN32)
            g_scale = static_cast<unsigned>(value);
#endif
        }
    }
#if defined(_WIN32)
    g_thread = std::thread(window_thread);
    std::unique_lock lock(g_mutex);
    g_ready.wait_for(lock, std::chrono::seconds(3), [] { return g_hwnd != nullptr || g_close_requested.load(); });
#endif
}

void window_present(const psprecomp::GuestMemory &memory, std::uint32_t address,
                    std::uint32_t stride, std::uint32_t format) {
    if (address == 0u || stride == 0u) return;
    const std::uint32_t bytes_per_pixel = format == 3u ? 4u : 2u;
    if (!memory.contains(address, static_cast<std::size_t>(stride) * kHeight * bytes_per_pixel))
        return;
    ++g_present_count;
    std::vector<std::uint32_t> frame(kWidth * kHeight, 0xFF000000u);
    for (std::uint32_t y = 0u; y < kHeight; ++y) {
        const std::uint32_t row = address + y * stride * bytes_per_pixel;
        for (std::uint32_t x = 0u; x < kWidth; ++x) {
            frame[static_cast<std::size_t>(y) * kWidth + x] =
                load_pixel(memory, row + x * bytes_per_pixel, format);
        }
    }

    if (std::getenv("PSPRECOMP_TOW1_FRAME_DIAG") != nullptr) {
        std::uint64_t non_black = 0u;
        std::uint32_t sample = 0u;
        for (const std::uint32_t pixel : frame) {
            if ((pixel & 0x00FFFFFFu) != 0u) {
                if (non_black == 0u) sample = pixel;
                ++non_black;
            }
        }
        std::cerr << "[tow1-frame] address=" << psprecomp::hex32(address)
                  << " stride=" << stride << " format=" << format
                  << " non_black=" << non_black << " sample="
                  << psprecomp::hex32(sample) << "\n";
    }

    if (const char *dump_path = std::getenv("PSPRECOMP_TOW1_FRAME_DUMP");
        dump_path != nullptr && *dump_path != '\0') {
        static std::uint32_t dump_count = 0u;
        std::uint32_t dump_at = 0u;
        if (const char *at_text = std::getenv("PSPRECOMP_TOW1_FRAME_DUMP_AT"))
            dump_at = static_cast<std::uint32_t>(std::strtoul(at_text, nullptr, 0));
        const bool dump_all = std::getenv("PSPRECOMP_TOW1_FRAME_DUMP_ALL") != nullptr;
        if (dump_all || dump_count == dump_at) {
            std::ofstream out(dump_path, std::ios::binary);
            if (out) {
                out << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
                for (const std::uint32_t pixel : frame) {
                    const char rgb[3] = {
                        static_cast<char>((pixel >> 16u) & 0xFFu),
                        static_cast<char>((pixel >> 8u) & 0xFFu),
                        static_cast<char>(pixel & 0xFFu)};
                    out.write(rgb, 3);
                }
            }
            if (std::getenv("PSPRECOMP_TOW1_EXIT_AFTER_FRAME_DUMP") != nullptr)
                std::exit(0);
        }
        ++dump_count;
    }

    if (g_post_osk_capture_armed.load() && g_post_osk_range_end != 0u) {
        const std::uint32_t offset = g_present_count - g_post_osk_capture_base;
        if (offset >= g_post_osk_range_start && offset <= g_post_osk_range_end &&
            ((offset - g_post_osk_range_start) % g_post_osk_range_step) == 0u) {
            const std::string path = g_post_osk_capture_path + "." +
                std::to_string(offset) + ".ppm";
            std::ofstream out(path, std::ios::binary);
            if (out) {
                out << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
                for (const std::uint32_t pixel : frame) {
                    const char rgb[3] = {
                        static_cast<char>((pixel >> 16u) & 0xFFu),
                        static_cast<char>((pixel >> 8u) & 0xFFu),
                        static_cast<char>(pixel & 0xFFu)};
                    out.write(rgb, 3);
                }
                std::cerr << "[tow1-frame] post-OSK range capture \""
                          << path << "\"\n";
            }
        }
        if (offset >= g_post_osk_range_end) {
            g_post_osk_capture_armed.store(false);
            if (std::getenv("PSPRECOMP_TOW1_POST_OSK_EXIT") != nullptr)
                std::exit(0);
        }
    } else if (g_post_osk_capture_armed.load() &&
               g_post_osk_capture_delay != 0u) {
        --g_post_osk_capture_delay;
    } else if (g_post_osk_capture_armed.load() &&
               g_post_osk_capture_delay == 0u &&
               g_post_osk_capture_armed.exchange(false)) {
        std::ofstream out(g_post_osk_capture_path, std::ios::binary);
        if (out) {
            out << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
            for (const std::uint32_t pixel : frame) {
                const char rgb[3] = {
                    static_cast<char>((pixel >> 16u) & 0xFFu),
                    static_cast<char>((pixel >> 8u) & 0xFFu),
                    static_cast<char>(pixel & 0xFFu)};
                out.write(rgb, 3);
            }
            std::cerr << "[tow1-frame] post-OSK capture \""
                      << g_post_osk_capture_path << "\"\n";
        }
        if (std::getenv("PSPRECOMP_TOW1_POST_OSK_EXIT") != nullptr)
            std::exit(0);
    }

    if (!g_enabled.load()) return;
    if (std::getenv("PSPRECOMP_TOW1_VSYNC") == nullptr ||
        std::strcmp(std::getenv("PSPRECOMP_TOW1_VSYNC"), "0") != 0) {
        using clock = std::chrono::steady_clock;
        static clock::time_point next_present = clock::now();
        constexpr auto frame_time = std::chrono::microseconds(16683);
        const clock::time_point now = clock::now();
        if (now < next_present) std::this_thread::sleep_until(next_present);
        next_present += frame_time;
        if (next_present < now) next_present = now + frame_time;
    }
    {
        std::lock_guard lock(g_mutex);
        g_frame = std::move(frame);
        g_have_frame = true;
    }
#if defined(_WIN32)
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, kPresentMessage, 0, 0);
#endif
}

void window_arm_post_osk_capture(const char *path) {
    if (path == nullptr || *path == '\0') return;
    g_post_osk_capture_path = path;
    g_post_osk_capture_delay = 0u;
    g_post_osk_capture_base = g_present_count;
    g_post_osk_range_start = 0u;
    g_post_osk_range_end = 0u;
    g_post_osk_range_step = 0u;
    if (const char *delay_text =
            std::getenv("PSPRECOMP_TOW1_POST_OSK_FRAME_DELAY")) {
        g_post_osk_capture_delay = static_cast<std::uint32_t>(
            std::strtoul(delay_text, nullptr, 0));
    }
    if (const char *start_text =
            std::getenv("PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_START")) {
        g_post_osk_range_start = static_cast<std::uint32_t>(
            std::strtoul(start_text, nullptr, 0));
        if (const char *end_text =
                std::getenv("PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_END"))
            g_post_osk_range_end = static_cast<std::uint32_t>(
                std::strtoul(end_text, nullptr, 0));
        if (const char *step_text =
                std::getenv("PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_STEP"))
            g_post_osk_range_step = static_cast<std::uint32_t>(
                std::strtoul(step_text, nullptr, 0));
        if (g_post_osk_range_step == 0u) g_post_osk_range_step = 1u;
    }
    g_post_osk_capture_armed.store(true);
}

void window_mark_post_osk() {
    g_post_osk_present_base = g_present_count;
}

std::uint32_t window_post_osk_present_count() {
    return g_present_count >= g_post_osk_present_base
        ? g_present_count - g_post_osk_present_base : 0u;
}

void window_schedule_post_osk_input(std::uint32_t mask,
                                    std::uint32_t delay_frames,
                                    std::uint32_t hold_frames) {
    if (mask == 0u || hold_frames == 0u) return;
    ScheduledInput input;
    input.start = g_present_count + delay_frames;
    input.end = input.start + hold_frames;
    input.mask = mask;
    g_scheduled_inputs.push_back(input);
}

void window_shutdown() {
    g_close_requested.store(true);
#if defined(_WIN32)
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
#endif
}

std::uint32_t window_buttons() {
    std::uint32_t buttons = g_buttons.load();
    for (const ScheduledInput &input : g_scheduled_inputs) {
        if (g_present_count >= input.start && g_present_count < input.end)
            buttons |= input.mask;
    }
    return buttons;
}

} // namespace tow1
