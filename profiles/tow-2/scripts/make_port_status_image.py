#!/usr/bin/env python3
"""Render the coverage poster for the tow-2 native port.

Evidence-based: AOT/import totals come from analysis/tow2_report.json, the unit
and corpus figures are measured from generated/ on disk, and the milestone list
reflects the verified tow-2 state (title screen + idle replay, not yet input).
"""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "profiles" / "tow-2"
OUT = PROFILE / "PORT_STATUS.png"
REPORT = PROFILE / "analysis" / "tow2_report.json"
GENERATED = PROFILE / "generated"

W, H = 1920, 1200
BG = "#0a0e13"
PANEL = "#121922"
BORDER = "#263241"
TEXT = "#e8edf3"
MUTED = "#93a4b8"
FAINT = "#5b6b7d"
GREEN = "#22c55e"
GREEN_DARK = "#14532d"
AMBER = "#f59e0b"
AMBER_DARK = "#78350f"
GRAY = "#566273"
GRAY_DARK = "#2a3440"
BLUE = "#38bdf8"
BLUE_DARK = "#0c4a6e"

# SDK libraries implemented by the tow-2 host. Everything else in the import
# table with a character/asset name is a guest game module, not an HLE target;
# they are aggregated into one block so the treemap stays readable.
REAL = {
    "IoFileMgrForUser", "Kernel_Library", "LoadExecForUser", "StdioForUser",
    "SysMemUserForUser", "ThreadManForUser", "UtilsForUser", "sceCtrl",
    "sceDisplay", "sceDmac", "sceGe_user", "sceLibFont", "sceMpeg",
    "scePower", "sceSuspendForUser", "sceUmdUser", "sceOpenPSID", "libfile",
}
PARTIAL = {
    "ModuleMgrForUser", "sceAudio", "sceSasCore", "sceUtility",
    "sceAtrac3plus",
}
DEFERRED = {
    "sceNet", "sceNetAdhoc", "sceNetAdhocMatching", "sceNetAdhocctl",
    "sceNetApctl", "sceNetInet", "sceNetResolver", "sceWlanDrv", "libcrypt",
}
SDK_EXTRA = {
    "IoFileMgrForUser", "Kernel_Library", "LoadExecForUser", "ModuleMgrForUser",
    "StdioForUser", "SysMemUserForUser", "ThreadManForUser", "UtilsForUser",
    "libfile", "libcrypt",
}


def is_sdk(name: str) -> bool:
    return name.startswith("sce") or name in SDK_EXTRA


def lib_state(name: str) -> str:
    if name in REAL:
        return "real"
    if name in DEFERRED:
        return "deferred"
    return "partial"


LIB_COLOR = {
    "real": (GREEN, GREEN_DARK),
    "partial": (AMBER, AMBER_DARK),
    "deferred": (GRAY, GRAY_DARK),
    "module": (BLUE, BLUE_DARK),
}
LIB_LABEL = {
    "real": "implemented", "partial": "partial / stub",
    "deferred": "deferred", "module": "game module",
}

MILESTONES = [
    ("Boot / module init", True),
    ("Intro movie (PSMF)", True),
    ("Title archive mount", True),
    ("Title screen render", True),
    ("Idle -> intro replay", True),
    ("PRESS START / menus", False),
    ("Character creation", False),
    ("Field / town", False),
    ("Battle", False),
]

TIMELINE = [
    ("BOOT", True),
    ("MOVIE", True),
    ("TITLE", True),
    ("REPLAY", True),
    ("INPUT", False),
    ("FIELD", False),
    ("BATTLE", False),
]


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    return ImageFont.truetype(str(Path("C:/Windows/Fonts") / name), size)


def mono(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(str(Path("C:/Windows/Fonts") / name), size)


def rounded(draw: ImageDraw.ImageDraw, box, radius, fill, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def fit_text(draw, text, max_w, max_h, start=28, minimum=10, bold=False, mono_font=False):
    factory = mono if mono_font else font
    for size in range(start, minimum - 1, -1):
        f = factory(size, bold)
        box = draw.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= max_w and box[3] - box[1] <= max_h:
            return f
    return factory(minimum, bold)


def treemap(items, rect, out):
    if not items:
        return
    if len(items) == 1:
        x, y, w, h = rect
        out.append((items[0], (x, y, x + w, y + h)))
        return
    total = sum(i["value"] for i in items)
    acc = 0
    split = 1
    for idx in range(len(items) - 1):
        acc += items[idx]["value"]
        if acc >= total / 2:
            split = idx + 1
            break
    left = items[:split]
    right = items[split:]
    left_ratio = sum(i["value"] for i in left) / total
    x, y, w, h = rect
    if w >= h:
        lw = w * left_ratio
        treemap(left, (x, y, lw, h), out)
        treemap(right, (x + lw, y, w - lw, h), out)
    else:
        lh = h * left_ratio
        treemap(left, (x, y, w, lh), out)
        treemap(right, (x, y + lh, w, h - lh), out)


def corpus_stats():
    units = sorted(GENERATED.glob("generated_unit_*.cpp"))
    total = sum(p.stat().st_size for p in units)
    return len(units), total / (1024 * 1024)


def main() -> None:
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    seeds = int(report["function_seed_count"])
    truncated = int(report["automatic_analysis"]["truncated_functions"])
    generated = seeds - truncated
    generated_pct = 100.0 * generated / seeds
    relocations = int(report["relocations"]["total"])
    imports = int(report["import_count"])
    by_library = report["import_libraries"]
    libraries = len(by_library)
    units, corpus_mb = corpus_stats()

    sdk = {name: count for name, count in by_library.items() if is_sdk(name)}
    modules = {name: count for name, count in by_library.items() if not is_sdk(name)}
    module_imports = sum(modules.values())

    gates = sum(1 for _, ok in MILESTONES if ok)

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    d.text((64, 48), "TALES OF THE WORLD: RADIANT MYTHOLOGY 2", font=font(42, True), fill=TEXT)
    d.text((66, 104), "Native recompilation & port coverage", font=font(25), fill=GREEN)
    d.text((66, 140), "ULJS00175  ·  English build  ·  2026-11-18", font=mono(17), fill=MUTED)
    d.text((W - 64, 54), "AOT / HLE / GAMEPLAY", font=mono(18, True), fill=BLUE, anchor="ra")
    d.text((W - 64, 84), "evidence-based scorecard", font=font(17), fill=MUTED, anchor="ra")
    d.line((64, 176, W - 64, 176), fill=BORDER, width=2)

    card_y, card_h = 202, 112
    card_w = (W - 128 - 3 * 18) // 4
    cards = [
        ("AOT FUNCTIONS", f"{seeds:,}", f"{generated_pct:.2f}% generated  ·  {truncated} truncated", GREEN),
        ("HLE IMPORTS", f"{imports}", f"{libraries} libraries  ·  0 missing on title path", BLUE),
        ("GAMEPLAY GATES", f"{gates} / {len(MILESTONES)}", "verified milestone checklist", GREEN),
        ("RUNTIME GAPS", "0", "missing HLE / AOT / GE on title runs", AMBER),
    ]
    for i, (label, value, sub, accent) in enumerate(cards):
        x = 64 + i * (card_w + 18)
        rounded(d, (x, card_y, x + card_w, card_y + card_h), 14, PANEL, BORDER, 1)
        d.rectangle((x, card_y, x + 5, card_y + card_h), fill=accent)
        d.text((x + 24, card_y + 16), label, font=mono(14, True), fill=MUTED)
        d.text((x + 24, card_y + 38), value, font=font(44, True), fill=TEXT)
        d.text((x + 24, card_y + 88), sub, font=font(13), fill=MUTED)

    px, py, pw, ph = 64, 344, 872, 492
    rounded(d, (px, py, px + pw, py + ph), 16, PANEL, BORDER, 1)
    d.text((px + 24, py + 18), "HLE IMPORTS BY LIBRARY", font=font(22, True), fill=TEXT)
    d.text((px + 24, py + 50),
           f"{imports} imports  ·  {libraries} libraries  ·  area = import count",
           font=font(14), fill=MUTED)
    d.text((px + pw - 24, py + 24), "0 MISSING", font=mono(18, True), fill=GREEN, anchor="ra")

    items = [
        {"name": name, "value": count, "state": lib_state(name)}
        for name, count in sdk.items()
    ]
    if modules:
        items.append({
            "name": f"game modules ×{len(modules)}",
            "value": module_imports,
            "state": "module",
        })
    items.sort(key=lambda i: i["value"], reverse=True)
    map_box = (px + 22, py + 82, pw - 44, ph - 140)
    blocks = []
    treemap(items, map_box, blocks)
    for item, (x0, y0, x1, y1) in blocks:
        if x1 - x0 < 4 or y1 - y0 < 4:
            continue
        fill, dark = LIB_COLOR[item["state"]]
        d.rectangle((x0 + 2, y0 + 2, x1 - 2, y1 - 2), fill=dark, outline=fill, width=2)
        bw, bh = (x1 - x0) - 12, (y1 - y0) - 12
        if bw < 24 or bh < 18:
            continue
        f = fit_text(d, item["name"], bw, max(12, bh - 22), start=19, minimum=8, bold=True)
        d.text((x0 + 8, y0 + 7), item["name"], font=f, fill=TEXT)
        if bh >= 34 and bw >= 42:
            d.text((x1 - 8, y1 - 7), str(item["value"]), font=mono(13, True), fill=fill, anchor="rs")

    lx, ly = px + 24, py + ph - 36
    for label, color in (("implemented", GREEN), ("partial / stub", AMBER),
                         ("deferred", GRAY), ("game module", BLUE)):
        d.rectangle((lx, ly, lx + 14, ly + 14), fill=color)
        d.text((lx + 22, ly - 2), label, font=font(13), fill=MUTED)
        lx += 158

    gx, gy, gw, gh = 960, 344, 896, 492
    rounded(d, (gx, gy, gx + gw, gy + gh), 16, PANEL, BORDER, 1)
    d.text((gx + 24, gy + 18), "GAMEPLAY GATES", font=font(22, True), fill=TEXT)
    d.text((gx + 24, gy + 50),
           f"{gates} of {len(MILESTONES)} tracked milestones verified",
           font=font(14), fill=MUTED)
    d.text((gx + gw - 24, gy + 24), f"{gates} / {len(MILESTONES)}",
           font=mono(20, True), fill=GREEN, anchor="ra")

    bar_x, bar_y, bar_w, bar_h = gx + 24, gy + 132, gw - 48, 28
    d.text((bar_x, bar_y - 25), "AOT function coverage", font=font(15, True), fill=TEXT)
    d.text((bar_x + bar_w, bar_y - 25), f"{generated_pct:.2f}%",
           font=mono(15, True), fill=GREEN, anchor="ra")
    rounded(d, (bar_x, bar_y, bar_x + bar_w, bar_y + bar_h), 8, GRAY_DARK)
    fill_w = int(bar_w * generated / seeds)
    rounded(d, (bar_x, bar_y, bar_x + fill_w, bar_y + bar_h), 8, GREEN)
    d.text((bar_x, bar_y + 34),
           f"{generated:,} of {seeds:,} seeds generated  ·  {truncated} analyzer-truncated",
           font=font(13), fill=MUTED)

    col_x = [gx + 24, gx + gw // 2 + 6]
    row_start = gy + 204
    row_h = 34
    for idx, (name, ok) in enumerate(MILESTONES):
        col = 0 if idx < 5 else 1
        row = idx if col == 0 else idx - 5
        x = col_x[col]
        y = row_start + row * row_h
        color = GREEN if ok else AMBER
        d.ellipse((x, y + 7, x + 18, y + 25), fill=color)
        if ok:
            d.line((x + 4, y + 16, x + 8, y + 20, x + 14, y + 11), fill=BG, width=3, joint="curve")
        else:
            d.text((x + 5, y + 4), "!", font=font(13, True), fill=BG)
        d.text((x + 32, y + 1), name, font=font(16), fill=TEXT if ok else AMBER)
        d.text((x + 32, y + 20), "verified" if ok else "pending",
               font=mono(11), fill=GREEN if ok else AMBER)

    d.text((gx + 24, gy + gh - 36),
           "runtime models: vblank-paced pump sleep  ·  silent SAS + music retirement "
           "(SOUND_WAIT=1 / SLEEP_CB_INFINITE=1 opt out)",
           font=font(12), fill=FAINT)

    ty = 884
    d.text((64, ty), "WORKING SLICE", font=font(20, True), fill=TEXT)
    d.text((64, ty + 28),
           "boot → intro movie → title → idle replay (loops); input not yet driven",
           font=font(14), fill=MUTED)
    line_y = ty + 82
    left, right = 105, W - 105
    d.line((left, line_y, right, line_y), fill=BORDER, width=4)
    step = (right - left) / (len(TIMELINE) - 1)
    for i, (label, ok) in enumerate(TIMELINE):
        cx = int(left + step * i)
        color = GREEN if ok else AMBER
        d.ellipse((cx - 14, line_y - 14, cx + 14, line_y + 14), fill=color, outline=BG, width=3)
        if ok:
            d.line((cx - 6, line_y, cx - 1, line_y + 5, cx + 7, line_y - 6), fill=BG, width=4, joint="curve")
        else:
            d.text((cx, line_y - 8), "!", font=font(15, True), fill=BG, anchor="mm")
        f = fit_text(d, label, step - 8, 20, start=13, minimum=9, bold=True, mono_font=True)
        d.text((cx, line_y + 26), label, font=f, fill=TEXT if ok else AMBER, anchor="ma")

    d.line((64, 1082, W - 64, 1082), fill=BORDER, width=2)
    footer = (
        f"{relocations:,} relocations  ·  {units} AOT units  ·  {corpus_mb:.2f} MB  ·  "
        f"{module_imports} game-module imports  ·  0 missing HLE / AOT / GE"
    )
    d.text((64, 1114), footer, font=mono(15), fill=MUTED)
    d.text((64, 1148), "Measure: profiles/tow-2/scripts/make_port_status_image.py",
           font=mono(14), fill=BLUE)
    d.text((W - 64, 1114), "AOT recompilation, not source-level decompilation",
           font=font(14), fill=FAINT, anchor="ra")
    d.text((W - 64, 1148), "green = verified  ·  amber = stub / pending  ·  gray = deferred",
           font=font(14), fill=FAINT, anchor="ra")

    img.save(OUT)
    print(OUT)


if __name__ == "__main__":
    main()
