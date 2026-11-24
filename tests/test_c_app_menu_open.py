#!/usr/bin/env python3
"""A2.1: src/app/{app_menu,app_open}.c versus frozen x86_64 assembly oracles.

AMO1 differential (x86_64):
    - app_menu: App_cmd_usermenu, App_dispatch_action (all 19 ACT_* + out-of-range),
      App_exec_menu_tmpl (empty cursor, normal entry, path_join overflow),
      App_run_pkg_action (fg, bg OK with menu / fallback name, bg error,
      pkg_ensure_exec fail, chdir ENOENT, marked list temp file + unlink,
      build_argv <= 0, path_join overflow)
    - app_open: App_open_selected_entry (NULL entry, FE_IS_PARENT normal & VFS
      parent URI & VFS exit with/without saved arc_ctx, VFS '..' entry by name,
      FE_IS_DIR & FE_LINK_DIR normal/error & VFS/error, file in virtual_mode 1/2/0,
      duoprc open rule vs ViewerDialog), App_reload_panes & reload_both_panes
AMO2 qemu-aarch64 matches x86_64 byte for byte
AMO3 mutation check (16 mutants across both C modules)
"""

import subprocess
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRCS = {
    "menu": "src/app/app_menu.c",
    "open": "src/app/app_open.c",
}
REFS = [
    "tests/harness/app_menu_reference.s",
    "tests/harness/app_open_reference.s",
]
CORE = [
    "src/core/oop_rt.c", "src/core/string.c", "src/core/string_path.c", "src/core/utf8.c",
    "src/core/memory.c", "src/core/mem_prims.c",
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
            "--redefine-sym", "FilePane_get_selected_entry=fake_FilePane_get_selected_entry",
            "--redefine-sym", "DirModel_entry_name=fake_DirModel_entry_name",
            "--redefine-sym", "FilePane_reload_keep_cursor=fake_FilePane_reload_keep_cursor",
            "--redefine-sym", "FilePane_enter_parent=fake_FilePane_enter_parent",
            "--redefine-sym", "FilePane_ensure_visible=fake_FilePane_ensure_visible",
            "--redefine-sym", "FilePane_nav_to=fake_FilePane_nav_to",
            "--redefine-sym", "pkg_ensure_exec=fake_pkg_ensure_exec",
            "--redefine-sym", "pkg_action_has_marked=fake_pkg_action_has_marked",
            "--redefine-sym", "pkg_write_marked_list=fake_pkg_write_marked_list",
            "--redefine-sym", "pkg_action_build_argv=fake_pkg_action_build_argv",
            "--redefine-sym", "sys_exec_argv=fake_sys_exec_argv",
            "--redefine-sym", "sys_exec_argv_bg=fake_sys_exec_argv_bg",
            "--redefine-sym", "pkg_vfs_parent_uri=fake_pkg_vfs_parent_uri",
            "--redefine-sym", "pkg_vfs_load_dir=fake_pkg_vfs_load_dir",
            "--redefine-sym", "app_jump_virtual_entry=fake_app_jump_virtual_entry",
            "--redefine-sym", "config_lookup_open=fake_config_lookup_open",
        ]
        for o in objs:
            subprocess.run(["objcopy", *redefs, str(o)], check=True, capture_output=True)
        extra += objs
    ovr = ovr or {}
    defs = [
        f'-DMENU_SRC="{ovr.get("menu", ROOT / SRCS["menu"])}"',
        f'-DOPEN_SRC="{ovr.get("open", ROOT / SRCS["open"])}"',
    ]
    return H.build(arch, t, ["tests/harness/app_menu_open_cli.c", *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *defs])


def build_script(work: Path) -> str:
    d_act = work / "act"
    d_act.mkdir(exist_ok=True)
    missing = work / "no_such_dir"
    mk = work / "marked.list"
    return "\n".join([
        # app_menu
        "M 0",
        *[f"M 1 {i}" for i in range(21)],
        "M 2 0 0 foo.txt echo:%f",         # NULL entry
        "M 2 1 0 foo.txt echo:%f",         # normal entry
        "M 2 1 1 longname.txt echo:%f",    # path_join overflow -> skip
        # M 3 <has_fe> <long_path> <ensure_rc> <has_mk> <build_argc> <bg_rc> <mode> <menu_lbl> <dir> <mk_file>
        f"M 3 1 0 0 0 1 0 - MyMenu {d_act} {mk}",          # fg normal
        f"M 3 1 0 0 1 1 0 bg MyMenu {d_act} {mk}",         # bg OK with menu + marked file unlinked
        f"M 3 1 0 0 0 1 0 bg - {d_act} {mk}",              # bg OK fallback to act.name
        f"M 3 1 0 0 0 1 1 bg MyMenu {d_act} {mk}",         # bg non-zero -> pkg_err
        f"M 3 1 0 -1 0 1 0 - MyMenu {d_act} {mk}",         # pkg_ensure_exec fail
        f"M 3 1 0 0 0 1 0 - MyMenu {missing} {mk}",        # chdir ENOENT
        f"M 3 0 0 0 1 0 0 - MyMenu {d_act} {mk}",          # build_argv <= 0, still unlinks & reloads
        f"M 3 1 1 0 0 1 0 - MyMenu {d_act} {mk}",          # path_join overflow -> early return
        # app_open: O 0 <has_fe> <flags> <vmode> <vfs_par> <vfs_load_rc> <nav_rc> <has_arc_ctx> <has_rule> <name>
        "O 0 0 0 0 0 0 0 0 0 none",        # NULL entry
        "O 0 1 32 0 0 0 0 0 0 ..",         # FE_IS_PARENT normal
        "O 0 1 32 3 1 0 0 0 0 ..",         # FE_IS_PARENT VFS parent URI
        "O 0 1 32 3 0 0 0 1 0 ..",         # FE_IS_PARENT VFS exit with saved path
        "O 0 1 32 3 0 0 0 0 0 ..",         # FE_IS_PARENT VFS exit without saved path
        "O 0 1 0 3 1 0 0 0 0 ..",          # VFS non-flag '..' -> parent path
        "O 0 1 1 0 0 0 0 0 0 sub",         # FE_IS_DIR normal OK
        "O 0 1 64 0 0 0 -13 0 0 lnkdir",   # FE_LINK_DIR normal -EACCES
        "O 0 1 1 3 0 0 0 0 0 vsub",        # FE_IS_DIR VFS OK
        "O 0 1 1 3 0 -5 0 0 0 vbad",       # FE_IS_DIR VFS -EIO
        "O 0 1 0 1 0 0 0 0 0 hit.txt",     # file in virtual_mode 1 -> jump
        "O 0 1 0 2 0 0 0 0 0 arc.txt",     # file in virtual_mode 2 -> no-op
        "O 0 1 0 0 0 0 0 0 1 doc.pdf",       # file in normal mode with duoprc open rule
        "O 0 1 0 0 0 0 0 0 0 readme.md",     # file in normal mode -> ViewerDialog
        # App_reload_panes / reload_both_panes
        "O 1 0 focus.txt",
        "O 1 1 -",
    ]) + "\n"


def test_differential(exe: Path, t: Path) -> str:
    s = build_script(t / "w").encode()
    out = H.diff_impls(exe, stdin=s).decode()
    for needle in ("call UserMenuDialog_new 1", "call App_cmd_shell 1",
                   "call sys_exec_argv |/bin/fake_tool|",
                   "call sys_exec_argv_bg |/bin/fake_tool|",
                   "m=|MyMenu|", "m=|pkg_name|", "E_NOENT",
                   "PA ff=1 cwd=1 mk_left=0",
                   "call pkg_vfs_parent_uri |vfs://root/sub|",
                   "call FilePane_nav_to p=1 to=|/saved/path|",
                   "call app_jump_virtual_entry 1 p=1 |/act/hit.txt|",
                   "call App_run_expanded_cmd 1 t=|open:%f| n=|doc.pdf| p=|/act/doc.pdf|",
                   "call ViewerDialog_new 1 p=|/act/readme.md|",
                   "call FilePane_reload_keep_cursor p=1 f=|focus.txt|"):
        assert needle in out, f"coverage missing: {needle}"
    print(f"[PASS] AMO1. app_menu & app_open match frozen asm byte for byte ({len(out.splitlines())} lines)")
    return out


def test_mutations(t: Path, baseline: str) -> None:
    s = build_script(t / "w").encode()
    muts = [
        ("menu", "disp-refresh", "case ACT_REFRESH: App_reload_panes(app, nullptr); break;",
         "case ACT_REFRESH: break;"),
        ("menu", "disp-shell", "case ACT_SHELL: App_cmd_shell(app); break;",
         "case ACT_SHELL: App_cmd_quit(app); break;"),
        ("menu", "tmpl-overflow", "if (path_join(full, m->path, name, PATH_MAX) < 0) return;",
         "if (path_join(full, m->path, name, PATH_MAX) < -9999) return;"),
        ("menu", "ensure-err", "if (pkg_ensure_exec(argv[0]) < 0) {",
         "if (pkg_ensure_exec(argv[0]) > 99) {"),
        ("menu", "bg-detect", "if (act->mode[0] == 'b' && act->mode[1] == 'g' && act->mode[2] == '\\0') {",
         "if (act->mode[0] == 'x' && act->mode[1] == 'g' && act->mode[2] == '\\0') {"),
        ("menu", "bg-fallback", "const char *lbl = act->menu[0] ? act->menu : act->name;",
         "const char *lbl = act->name;"),
        ("menu", "bg-rc", "if (r != 0) {", "if (r == 99) {"),
        ("menu", "mk-unlink", "if (buf->tmp_marked[0] != '\\0') (void)os_unlinkat(AT_FDCWD, buf->tmp_marked, 0);",
         ""),
        ("open", "vfs-dotdot", "if (m->virtual_mode == 3 && name[0] == '.' && name[1] == '.' && name[2] == '\\0')",
         "if (0 && m->virtual_mode == 3 && name[0] == '.' && name[1] == '.' && name[2] == '\\0')"),
        ("open", "vfs-par-cur", "pane->cursor = 0; pane->scroll = 0;",
         "pane->cursor = 1; pane->scroll = 0;"),
        ("open", "vfs-exit-vm", "m->virtual_mode = 0;", "m->virtual_mode = 3;"),
        ("open", "dir-flag", "if (fe->flags & (FE_IS_DIR | FE_LINK_DIR)) {",
         "if (fe->flags & FE_IS_DIR) {"),
        ("open", "vfs-dir-err", "if (rc < 0) { App_set_status(app, fs_errno_message(rc), MSG_ERR); return; }",
         "if (rc < -999) { App_set_status(app, fs_errno_message(rc), MSG_ERR); return; }"),
        ("open", "vm1-jump", "if (m->virtual_mode == 1) { app_jump_virtual_entry(app, pane, full); return; }",
         "if (m->virtual_mode == 9) { app_jump_virtual_entry(app, pane, full); return; }"),
        ("open", "open-rule", "if (tmpl) { App_run_expanded_cmd(app, tmpl, name, full); return; }",
         "if (!tmpl && name[0] == 1) { App_run_expanded_cmd(app, tmpl, name, full); return; }"),
        ("open", "reload-inact", "FilePane_reload_keep_cursor(get_inactive_pane(app), nullptr);", ""),
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
    print(f"[PASS] AMO3. {len(muts)}/{len(muts)} mutants killed across app_menu & app_open")


def test_archive_first(exe: Path) -> None:
    """A double click (App_open_selected_entry) asks the archive layer first: handled -> no viewer; not handled -> the viewer as before (C only)."""
    out = H.run_impl("x86_64", exe, "c", stdin=b"O 2 1\nO 2 0\n").stdout.decode().splitlines()
    assert [l for l in out if l.startswith("OA")] == ["OA dlg=0", "OA dlg=1"], out
    assert out.count("call ViewerDialog_new 1 p=|/act/a.zip|") == 1, out           # only the unhandled entry reaches the viewer
    print("[PASS] AM1b. double click / Enter in a pane: the archive layer gets the entry first, a handled archive opens no viewer")


def main() -> None:
    with H.workdir() as wd:
        t = Path(wd)
        (t / "w").mkdir()
        exe = build("x86_64", t / "x86")
        out = test_differential(exe, t)
        test_archive_first(exe)
        if H.available("aarch64"):
            exe_arm = build("aarch64", t / "arm64")
            r = H.run_impl("aarch64", exe_arm, "c", stdin=build_script(t / "w").encode())
            assert r.returncode == 0 and r.stdout.decode() == out, "aarch64 differs"
            print("[PASS] AMO2. qemu-aarch64 matches x86_64 byte for byte")
        test_mutations(t, out)


if __name__ == "__main__":
    main()
