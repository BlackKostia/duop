#!/usr/bin/env python3
"""C struct/vtable layouts (include/duop_*.h) versus the assembler's view (include/classes.inc, config.inc):
every offset and size is evaluated by both compilers and must agree on x86_64 and aarch64 (64-bit targets)."""

import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

LAYOUTS = [  # (C header, C expression, assembler symbol)
    ("duop_oop.h", "sizeof(Object)", "Object._size"),
    ("duop_oop.h", "offsetof(Object, vptr)", "Object.vptr"),
    ("duop_oop.h", "sizeof(Widget)", "Widget._size"),
    *[("duop_oop.h", f"offsetof(Widget, {f})", f"Widget.{f}") for f in ("parent", "x", "y", "w", "h", "flags", "id")],
    *[("duop_oop.h", f"offsetof(ObjectVtbl, {f})", f"ObjectVtbl.{f}") for f in ("super", "class_name", "inst_size", "dtor")],
    *[("duop_oop.h", f"offsetof(WidgetVtbl, {f})", f"WidgetVtbl.{f}")
      for f in ("layout", "render", "on_key", "on_mouse", "on_focus", "hit_test")],
    ("duop_oop.h", "sizeof(WidgetVtbl)", "WidgetVtbl._size"),
    ("duop_oop.h", "WF_VISIBLE", "WF_VISIBLE"),
    ("duop_oop.h", "WF_FOCUSED", "WF_FOCUSED"),
    ("duop_oop.h", "WF_DIRTY", "WF_DIRTY"),
    ("duop_oop.h", "WF_ENABLED", "WF_ENABLED"),
    ("duop_ui.h", "sizeof(DialogVtbl)", "DialogVtbl._size"),
    ("duop_ui.h", "offsetof(DialogVtbl, on_confirm)", "DialogVtbl.on_confirm"),
    ("duop_ui.h", "offsetof(DialogVtbl, on_cancel)", "DialogVtbl.on_cancel"),
    *[("duop_ui.h", k, k) for k in ("DLG_OPEN", "DLG_OK", "DLG_CANCEL")],
    ("duop_config.h", "PATH_MAX", "PATH_MAX"),
    ("duop_fs.h", "DC_SLOTS", "DC_SLOTS"),
    ("duop_task.h", "offsetof(TaskShared, total_bytes)", "TaskShared.total_bytes"),
    ("duop_task.h", "offsetof(TaskShared, total_files)", "TaskShared.total_files"),
    ("duop_task.h", "offsetof(TaskShared, done_bytes)", "TaskShared.done_bytes"),
    ("duop_task.h", "offsetof(TaskShared, done_files)", "TaskShared.done_files"),
    ("duop_task.h", "offsetof(TaskShared, result)", "TaskShared.result"),
    ("duop_task.h", "offsetof(TaskShared, seq)", "TaskShared.seq"),
    ("duop_task.h", "offsetof(TaskShared, phase)", "TaskShared.phase"),
    ("duop_task.h", "offsetof(TaskShared, cancel)", "TaskShared.cancel"),
    ("duop_task.h", "offsetof(TaskShared, conflict_state)", "TaskShared.conflict_state"),
    ("duop_task.h", "offsetof(TaskShared, skipped)", "TaskShared.skipped"),
    ("duop_task.h", "offsetof(TaskShared, conflict_action)", "TaskShared.conflict_action"),
    ("duop_task.h", "offsetof(TaskShared, conflict_all)", "TaskShared.conflict_all"),
    ("duop_task.h", "offsetof(TaskShared, conflict_src_sz)", "TaskShared.conflict_src_sz"),
    ("duop_task.h", "offsetof(TaskShared, conflict_dst_sz)", "TaskShared.conflict_dst_sz"),
    ("duop_task.h", "offsetof(TaskShared, cur_name)", "TaskShared.cur_name"),
    ("duop_task.h", "offsetof(TaskShared, conflict_name)", "TaskShared.conflict_name"),
    ("duop_task.h", "sizeof(TaskShared)", "TaskShared._size"),
    ("duop_task.h", "offsetof(TaskJob, names_len)", "TaskJob.names_len"),
    ("duop_task.h", "offsetof(TaskJob, op)", "TaskJob.op"),
    ("duop_task.h", "offsetof(TaskJob, count)", "TaskJob.count"),
    ("duop_task.h", "offsetof(TaskJob, join_dst)", "TaskJob.join_dst"),
    ("duop_task.h", "offsetof(TaskJob, pad)", "TaskJob.pad"),
    ("duop_task.h", "offsetof(TaskJob, src_dir)", "TaskJob.src_dir"),
    ("duop_task.h", "offsetof(TaskJob, dst)", "TaskJob.dst"),
    ("duop_task.h", "offsetof(TaskJob, names)", "TaskJob.names"),
    ("duop_task.h", "sizeof(TaskJob)", "TaskJob._size"),
    ("duop_task.h", "offsetof(TaskSnap, total_bytes)", "TaskSnap.total_bytes"),
    ("duop_task.h", "offsetof(TaskSnap, total_files)", "TaskSnap.total_files"),
    ("duop_task.h", "offsetof(TaskSnap, done_bytes)", "TaskSnap.done_bytes"),
    ("duop_task.h", "offsetof(TaskSnap, done_files)", "TaskSnap.done_files"),
    ("duop_task.h", "offsetof(TaskSnap, elapsed_ms)", "TaskSnap.elapsed_ms"),
    ("duop_task.h", "offsetof(TaskSnap, speed_bps)", "TaskSnap.speed_bps"),
    ("duop_task.h", "offsetof(TaskSnap, eta_sec)", "TaskSnap.eta_sec"),
    ("duop_task.h", "offsetof(TaskSnap, op)", "TaskSnap.op"),
    ("duop_task.h", "offsetof(TaskSnap, phase)", "TaskSnap.phase"),
    ("duop_task.h", "offsetof(TaskSnap, paused)", "TaskSnap.paused"),
    ("duop_task.h", "offsetof(TaskSnap, percent)", "TaskSnap.percent"),
    ("duop_task.h", "offsetof(TaskSnap, canceling)", "TaskSnap.canceling"),
    ("duop_task.h", "offsetof(TaskSnap, pad)", "TaskSnap.pad"),
    ("duop_task.h", "offsetof(TaskSnap, cur_name)", "TaskSnap.cur_name"),
    ("duop_task.h", "sizeof(TaskSnap)", "TaskSnap._size"),
    ("duop_task.h", "TASK_OP_COPY", "TASK_OP_COPY"),
    ("duop_task.h", "TASK_OP_MOVE", "TASK_OP_MOVE"),
    ("duop_task.h", "TASK_OP_DELETE", "TASK_OP_DELETE"),
    ("duop_task.h", "TASK_PHASE_SCAN", "TASK_PHASE_SCAN"),
    ("duop_task.h", "TASK_PHASE_RUN", "TASK_PHASE_RUN"),
    ("duop_task.h", "TASK_PHASE_DONE", "TASK_PHASE_DONE"),
    ("duop_task.h", "TASK_SHM_SIZE", "TASK_SHM_SIZE"),
    ("duop_fs.h", "DC_BUDGET", "DC_BUDGET"),
    ("duop_config.h", "sizeof(Config)", "15024"),                       # CFG_TOTAL_SZ (private to the old config*.s)
    ("duop_config.h", "offsetof(Config, color_mask)", "16"),
    ("duop_config.h", "offsetof(Config, bind_cnt)", "20"),
    ("duop_config.h", "offsetof(Config, colors)", "32"),
    ("duop_config.h", "offsetof(Config, binds)", "160"),
    ("duop_config.h", "offsetof(Config, opens)", "672"),
    ("duop_config.h", "offsetof(Config, menus)", "8864"),
    ("duop_config.h", "offsetof(Config, lang_id)", "CFG_LANG_ID"),
    ("duop_config.h", "I18N_ID_LEN", "I18N_ID_LEN"),
    ("duop_config.h", "MOD_ALT", "MOD_ALT"),
    ("duop_config.h", "MOD_CTRL", "MOD_CTRL"),
    ("duop_config.h", "KEY_F1", "KEY_F1"),
    ("duop_config.h", "KEY_F10", "KEY_F10"),
    ("duop_config.h", "KEY_F11", "KEY_F11"),
    ("duop_config.h", "KEY_F12", "KEY_F12"),
    ("duop_config.h", "SORT_NAME", "SORT_NAME"),
    ("duop_config.h", "SORT_SIZE", "SORT_SIZE"),
    ("duop_config.h", "SORT_MTIME", "SORT_MTIME"),
    ("duop_config.h", "SORT_EXT", "SORT_EXT"),
    ("duop_tui.h", "sizeof(Cell)", "Cell._size"),
    *[("duop_tui.h", f"offsetof(Cell, {f})", f"Cell.{f}") for f in ("ch", "fg", "bg", "attrs")],
    ("duop_tui.h", "sizeof(Theme)", "Theme._size"),
    *[("duop_tui.h", f"offsetof(Theme, {f})", f"Theme.{f}") for f in ("name", "bg", "surface", "accent", "text", "dir_fg", "hidden_fg",
                                                                  "badge_fg", "dialog_border", "err_fg", "scroll_track", "scroll_thumb", "pad")],
    ("duop_tui.h", "sizeof(ScreenBuffer)", "ScreenBuffer._size"),
    *[("duop_tui.h", f"offsetof(ScreenBuffer, {f})", f"ScreenBuffer.{f}")
      for f in ("front", "back", "out_buf", "out_len", "out_cap", "cell_cap", "width", "height", "force_full", "pen_fg", "pen_bg", "pen_attrs")],
    ("duop_tui.h", "sizeof(DataSpec)", "DataSpec._size"),
    *[("duop_tui.h", f"offsetof(DataSpec, {f})", f"DataSpec.{f}") for f in ("env", "sub", "ext", "load")],
    ("duop_tui.h", "sizeof(Event)", "Event._size"),
    *[("duop_tui.h", f"offsetof(Event, {f})", f"Event.{f}") for f in ("type", "mods", "key", "mouse_x", "mouse_y", "mouse_btn", "mouse_kind", "lat")],
    *[("duop_keys.h", k, k) for k in ("EVT_KEY", "EVT_MOUSE", "EVT_RESIZE", "MOD_SHIFT", "KEY_DOWN", "KEY_PGDN", "KEY_DELETE", "KEY_BTAB",
                                      "KEY_ENTER", "KEY_ESC", "KEY_F2", "KEY_F9", "MOUSE_NONE", "MOUSE_WHEEL_UP", "MOUSE_WHEEL_DOWN",
                                      "MACT_PRESS", "MACT_RELEASE", "MACT_DRAG", "MACT_MOVE", "MACT_WHEEL")],
    ("duop_tui.h", "IMG_PROTO_QUAD", "IMG_PROTO_QUAD"),
    ("duop_tui.h", "IMG_PROTO_HALF", "IMG_PROTO_HALF"),
    ("duop_tui.h", "IMG_PROTO_SIXEL", "IMG_PROTO_SIXEL"),
    ("duop_tui.h", "IMG_PROTO_KITTY", "IMG_PROTO_KITTY"),
    ("duop_tui.h", "sizeof(LangStrings)", "LangStrings._size"),
    ("duop_tui.h", "offsetof(LangStrings, s[1])", "LangStrings.name"),
    ("duop_tui.h", "I18N_MAX", "I18N_MAX"),
    ("duop_tui.h", "I18N_FILE_MAX", "I18N_FILE_MAX"),
    ("duop_tui.h", "I18N_ARENA", "I18N_ARENA"),
    ("duop_tui.h", "I18N_ID_LEN", "I18N_ID_LEN"),
    ("duop_img.h", "sizeof(Image)", "Image._size"),
    *[("duop_img.h", f"offsetof(Image, {f})", f"Image.{f}") for f in ("px", "w", "h", "fmt")],
    *[("duop_img.h", k, k) for k in ("IMG_FMT_PNG", "IMG_FMT_JPEG", "IMG_FMT_GIF", "IMG_FMT_BMP", "IMG_FMT_PNM", "IMG_FMT_TGA", "IMG_FMT_QOI",
                                     "IMG_FMT_WEBP", "IMG_MAX_DIM", "IMG_MAX_PIXELS", "IMG_E_FORMAT", "IMG_E_UNSUP", "IMG_E_CORRUPT", "IMG_E_NOMEM")],
    ("duop_fs.h", "sizeof(FileEntry)", "FileEntry._size"),
    *[("duop_fs.h", f"offsetof(FileEntry, {f})", f"FileEntry.{f}") for f in ("size", "mtime", "mode", "flags", "name_off", "name_len", "disp_width", "ext_offset")],
    ("duop_fs.h", "sizeof(NavTab)", "4384"),                              # TAB_SIZE of the pane code (file_pane*.s FP_TAB_*)
    ("duop_fs.h", "offsetof(NavTab, cursor)", "4096"),
    ("duop_fs.h", "offsetof(NavTab, scroll)", "4100"),
    ("duop_fs.h", "offsetof(NavTab, sort_mode)", "4104"),
    ("duop_fs.h", "offsetof(NavTab, sort_desc)", "4108"),
    ("duop_fs.h", "offsetof(NavTab, show_hidden)", "4112"),
    ("duop_fs.h", "offsetof(NavTab, hist_pos)", "4116"),
    ("duop_fs.h", "offsetof(NavTab, hist_len)", "4120"),
    ("duop_fs.h", "offsetof(NavTab, hist)", "4128"),
    ("duop_du.h", "sizeof(DuProgress)", "DuProgress._size"),
    *[("duop_du.h", f"offsetof(DuProgress, {f})", f"DuProgress.{f}") for f in ("start_ms", "dirs", "files", "bytes", "done", "total", "cancel", "phase")],
    ("duop_du.h", "DU_PHASE_RUN", "DU_PHASE_RUN"),
    ("duop_du.h", "DU_PHASE_DONE", "DU_PHASE_DONE"),
    ("duop_fs.h", "DIRMODEL_SIZE", "DirModel._size"),
    *[("duop_fs.h", f"offsetof(DirModel, {f})", f"DirModel.{f}")
      for f in ("entries", "order", "name_pool", "count", "raw_count", "raw_cap", "name_used", "name_cap", "total_bytes", "load_buf", "load_pos", "load_bytes",
                "load_dirfd", "is_loading", "load_aborted", "huge_mode", "is_unsorted", "stats_complete", "dir_count", "file_count", "sort_mode", "sort_desc",
                "show_hidden", "has_dotdot", "dir_dev", "dir_ino", "dir_mtime", "dir_mtime_ns", "dir_pad", "flt_raw", "flt_lc_len", "flt_valid", "flt_hidden",
                "virtual_mode", "du_mode", "du_total", "arc_ctx", "git_ctx", "path", "filter", "flt_lc", "flt_applied")],
    *[("duop_fs.h", k, k) for k in ("FE_IS_DIR", "FE_IS_LINK", "FE_IS_EXEC", "FE_IS_HIDDEN", "FE_SELECTED", "FE_IS_PARENT", "FE_LINK_DIR", "FE_STAT_READY",
                                    "FE_DT_UNKNOWN", "FILTER_MAX", "DIRENTS_BUF_SIZE", "HUGE_DIR_THRESHOLD")],
    ("duop_tui.h", "KBD_MAP_MAX", "KBD_MAP_MAX"),
    ("duop_tui.h", "KBD_FILE_MAX", "KBD_FILE_MAX"),
    ("duop_tui.h", "KEY_UP", "KEY_UP"),
    ("duop_tui.h", "CELL_WIDE_TAIL", "CELL_WIDE_TAIL"),
]
TARGETS = {"x86_64": ["x86_64-linux-gnu"], "aarch64": ["aarch64-linux-gnu", "-mno-outline-atomics"]}


def nm_values(obj: Path, tool: str = "nm") -> dict[str, int]:
    out = {}
    for line in subprocess.run([tool, str(obj)], capture_output=True, text=True, check=True).stdout.splitlines():
        m = re.match(r"([0-9a-fA-F]+)\s+[aAnN]\s+(\S+)$", line)
        if m:
            v = int(m.group(1), 16)
            out[m.group(2)] = v - (1 << 64) if v >= 1 << 63 else v
    return out


def asm_values(tmp: Path) -> dict[str, int]:
    names = sorted({a for _, _, a in LAYOUTS})
    src = tmp / "layout.s"
    src.write_text('.intel_syntax noprefix\n.include "classes.inc"\n.section .text\n' +
                   "".join(f".globl L__{n.replace('.', '_')}\n.set L__{n.replace('.', '_')}, {n}\n" for n in names))
    obj = tmp / "layout_asm.o"
    subprocess.run(["as", "-I", str(ROOT / "include/os/linux/x86_64"), "-I", str(ROOT / "include"), str(src), "-o", str(obj)],
                   check=True, capture_output=True)
    vals = nm_values(obj)
    return {n: vals[f"L__{n.replace('.', '_')}"] for n in names}


def c_values(tmp: Path, triple: list[str]) -> dict[str, int]:
    heads = sorted({h for h, _, _ in LAYOUTS})
    body = "".join(f'    __asm__(".globl C__{i}\\n.set C__{i}, %c0" :: "i"((long long)({e})));\n' for i, (_, e, _) in enumerate(LAYOUTS))
    src = tmp / "layout.c"
    src.write_text("".join(f'#include "{h}"\n' for h in heads) + "void probe(void)\n{\n" + body + "}\n")
    obj = tmp / "layout_c.o"
    subprocess.run(["clang", f"--target={triple[0]}", *triple[1:], "-std=gnu2x", "-ffreestanding", "-O1",
                    "-Wno-unused-command-line-argument", "-I", str(ROOT / "include"), "-c", str(src), "-o", str(obj)],
                   check=True, capture_output=True)
    vals = nm_values(obj)
    return {a: vals[f"C__{i}"] for i, (_, _, a) in enumerate(LAYOUTS)}


def main() -> None:
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        asm = asm_values(tmp)
        for arch, triple in TARGETS.items():
            cv = c_values(tmp, triple)
            for name, val in cv.items():
                assert asm[name] == val, f"{arch}: {name}: asm={asm[name]} C={val}"
            print(f"[PASS] LY-{arch}: {len(cv)} struct/vtable offsets and sizes identical in C and assembler")
    print("ALL ABI LAYOUT TESTS PASSED.")


if __name__ == "__main__":
    main()
