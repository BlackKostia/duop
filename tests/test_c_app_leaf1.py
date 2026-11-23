#!/usr/bin/env python3
"""A1.1: src/app/{app_choosedir,app_arc,app_view,app_toggles}.c versus frozen x86_64
assembly oracles.

AL1 differential (x86_64):
    - app_choosedir: App_write_choosedir (NULL/empty/real file content with trailing \\n),
      app_mouse_hover (dual vs preview mode)
    - app_arc: arc_nav_up (<0 vs >=0), arc_handle_enter (0/1/2/3 incl. truncated status),
      arc_extract_selected (0 vs -1)
    - app_view: App_open_viewer (image -> ViewAsDialog, plain -> ViewerDialog),
      App_view_as_done (normal + OOM), App_cmd_view_mode (dual vs preview)
    - app_toggles: App_cmd_toggle_preview, App_cmd_toggle_hidden, App_cmd_cycle_theme,
      App_cmd_toggle_lang (wrap at i18n_count), App_cmd_quit (idle vs active task),
      App_dispatch_f_action (0..9 + out-of-range)
AL2 qemu-aarch64 matches x86_64 byte for byte
AL3 mutation check across all 4 C modules
"""

import subprocess
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRCS = {
    "cd": "src/app/app_choosedir.c",
    "arc": "src/app/app_arc.c",
    "vw": "src/app/app_view.c",
    "tg": "src/app/app_toggles.c",
}
REFS = [
    "tests/harness/app_choosedir_reference.s",
    "tests/harness/app_arc_reference.s",
    "tests/harness/app_view_reference.s",
    "tests/harness/app_toggles_reference.s",
]
CORE = [
    "src/core/oop_rt.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c",
    "src/core/syscall.c", "src/core/mem_prims.c",
    "src/tui/screen.c", "src/tui/screen_buffer.c", "src/tui/screen_text.c", "src/tui/screen_rect.c",
    "build/i18n_en.c",
]


def build(arch: str, t: Path, ovr: dict[str, Path] | None = None) -> Path:
    t.mkdir(parents=True, exist_ok=True)
    inc = ["-I", str(ROOT / f"include/os/linux/{arch}"), "-I", str(ROOT / "include")]
    extra = []
    if arch == "x86_64":
        objs = []
        for r in REFS:
            o = t / (Path(r).stem + ".o")
            subprocess.run(["as", *inc, str(ROOT / r), "-o", str(o)], check=True, capture_output=True)
            objs.append(o)
        H.rename_defined_globals(objs, "ref_")
        redefs = [
            "--redefine-sym", "arc_nav_up_model=fake_arc_nav_up_model",
            "--redefine-sym", "arc_enter_model=fake_arc_enter_model",
            "--redefine-sym", "arc_extract_entry=fake_arc_extract_entry",
            "--redefine-sym", "FilePane_get_selected_entry=fake_FilePane_get_selected_entry",
            "--redefine-sym", "FilePane_ensure_visible=fake_FilePane_ensure_visible",
            "--redefine-sym", "DirModel_apply_filter=fake_DirModel_apply_filter",
            "--redefine-sym", "fs_read_file_head=fake_fs_read_file_head",
            "--redefine-sym", "img_detect=fake_img_detect",
            "--redefine-sym", "ViewAsDialog_new=fake_ViewAsDialog_new",
            "--redefine-sym", "ViewerDialog_new=fake_ViewerDialog_new",
            "--redefine-sym", "ViewerDialog_set_mode=fake_ViewerDialog_set_mode",
            "--redefine-sym", "PreviewPane_cycle_mode=fake_PreviewPane_cycle_mode",
            "--redefine-sym", "theme_get=fake_theme_get",
            "--redefine-sym", "i18n_count=fake_i18n_count",
            "--redefine-sym", "i18n_set_lang=fake_i18n_set_lang",
        ]
        for o in objs:
            subprocess.run(["objcopy", *redefs, str(o)], check=True, capture_output=True)
        extra += objs
    ovr = ovr or {}
    defs = [
        f'-DCD_SRC="{ovr.get("cd", ROOT / SRCS["cd"])}"',
        f'-DARC_SRC="{ovr.get("arc", ROOT / SRCS["arc"])}"',
        f'-DVW_SRC="{ovr.get("vw", ROOT / SRCS["vw"])}"',
        f'-DTG_SRC="{ovr.get("tg", ROOT / SRCS["tg"])}"',
    ]
    return H.build(arch, t, ["tests/harness/app_leaf1_cli.c", *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *defs])


def build_script(cd_file: Path) -> str:
    return "\n".join([
        # app_choosedir
        "W -", "W !", f"W {cd_file}",
        "H 0", "H 1",
        # app_arc
        "A 0 -1", "A 0 0", "A 0 3",
        "A 1 0 -1", "A 1 1 -1", "A 1 2 4", "A 1 3 -1",
        "A 2 0", "A 2 -1",
        # app_view
        "V 0 2", "V 0 0",
        "V 1 1 2", "V 1 0 2",
        "V 2 0", "V 2 1",
        # app_toggles
        "T 0", "T 0",
        "T 1", "T 1",
        "T 2", "T 2",
        "T 3", "T 3", "T 3",
        "T 4 1", "T 4 0",
        *[f"T 5 {i}" for i in range(11)],
    ]) + "\n"


def test_differential(exe: Path, t: Path) -> str:
    s = build_script(t / "choosedir.txt").encode()
    out = H.diff_impls(exe, stdin=s).decode()
    for needle in ("W file=|/chosen/dir\\x0a|", "call on_mouse id=2",
                   "AU rc=1 cur=3 scr=0", "AE rc=1 cur=4 rd=1", "AX rc=0 p=|/tmp/extracted|",
                   "call ViewAsDialog_new 1 fmt=2", "call ViewerDialog_set_mode 1 m=2",
                   "call PreviewPane_cycle_mode 1", "TP rm=1 rd=1", "TH l=1 r=1 rd=1",
                   "TL idx=0 ff=1 rd=1", "call App_task_confirm_quit 1", "TQ run=0",
                   "call App_cmd_help", "call App_cmd_delete"):
        assert needle in out, f"coverage missing: {needle}"
    print(f"[PASS] AL1. A1.1 leaf app modules match frozen asm byte for byte ({len(out.splitlines())} lines)")
    return out


def test_mutations(t: Path, baseline: str) -> None:
    s = build_script(t / "m_cd.txt").encode()
    muts = [
        ("cd", "cd-nl", "char nl = '\\n';", "char nl = ' ';"),
        ("cd", "hov-prev", "if (app->right_mode != RIGHT_MODE_PREVIEW)", "if (1)"),
        ("arc", "up-neg", "if (idx < 0) return 0;", "if (idx <= 0) return 0;"),
        ("arc", "ent-2", "if (r == 2) {", "if (0) {"),
        ("arc", "ent-3", "if (r == 3) App_set_status", "if (0) App_set_status"),
        ("arc", "ext-rc", "return (int)arc_extract_entry", "return -1; (void)arc_extract_entry"),
        ("vw", "img-va", "if (fmt) app->active_dialog = (uint64_t)(uintptr_t)ViewAsDialog_new(app, path, fmt);",
         "if (0) app->active_dialog = (uint64_t)(uintptr_t)ViewAsDialog_new(app, path, fmt);"),
        ("vw", "vad-mode", "ViewerDialog_set_mode(v, (int)dlg->choice);",
         "ViewerDialog_set_mode(v, 0);"),
        ("vw", "vm-prev", "if (app->right_mode == RIGHT_MODE_PREVIEW)", "if (0)"),
        ("tg", "prev-act", "if (app->right_mode == RIGHT_MODE_PREVIEW)", "if (0)"),
        ("tg", "hid-sync", "rm->show_hidden = lm->show_hidden;", "rm->show_hidden = 0;"),
        ("tg", "lang-wrap", "if (next >= (uint32_t)cnt) next = 0;", "if (next > (uint32_t)cnt) next = 0;"),
        ("tg", "quit-task", "if (app->task_active) { App_task_confirm_quit(app); return; }", ""),
        ("tg", "f-act", "case 7: App_cmd_delete(app); break;", "case 7: App_cmd_copy(app); break;"),
    ]
    survivors: list[str] = []
    txts = {k: (ROOT / v).read_text() for k, v in SRCS.items()}

    def check(idx: int, key: str, name: str, old: str, new: str) -> None:
        assert old in txts[key] and old != new, name
        mp = t / f"m{idx}" / Path(SRCS[key]).name
        mp.parent.mkdir(parents=True, exist_ok=True)
        mp.write_text(txts[key].replace(old, new, 1))
        exe = build("x86_64", t / f"mb{idx}", {key: mp})
        r = H.run_impl("x86_64", exe, "c", stdin=s)
        if r.returncode == 0 and r.stdout.decode() == baseline:
            survivors.append(name)

    H.parallel([lambda i=i, m=m: check(i, *m) for i, m in enumerate(muts)])
    assert not survivors, f"surviving mutants: {survivors}"
    print(f"[PASS] AL3. {len(muts)}/{len(muts)} mutants killed across A1.1 app modules")


def main() -> None:
    with H.workdir() as wd:
        t = Path(wd)
        exe = build("x86_64", t / "x86")
        out = test_differential(exe, t)
        if H.available("aarch64"):
            exe_arm = build("aarch64", t / "arm64")
            r = H.run_impl("aarch64", exe_arm, "c", stdin=build_script(t / "arm_cd.txt").encode())
            assert r.returncode == 0 and r.stdout.decode() == out, "aarch64 differs"
            print("[PASS] AL2. qemu-aarch64 matches x86_64 byte for byte")
        test_mutations(t, out)


if __name__ == "__main__":
    main()
