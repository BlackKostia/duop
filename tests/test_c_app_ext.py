#!/usr/bin/env python3
"""A2.4: src/app/app_ext.c versus frozen x86_64 assembly oracles (app_ext.s + app_ext_keys.s).

AE1 differential (x86_64):
    - app_jump_virtual_entry: nested path, root-level '/file', bare name, nav error,
      find hit vs miss
    - App_close_dialog_ext: OP_USERMENU (config item found/missing, pkg item found/missing),
      OP_BOOKMARKS & OP_FRECENCY (NULL, OK, nav -errno), OP_CONFLICT, OP_SEARCH (0 hits,
      hits with has_dotdot & count>1, hits without dotdot, negative error), OP_CHMOD,
      OP_SYMLINK, OP_HARDLINK, OP_PALETTE (0, builtin, pkg OK/NULL), OP_FUZZY (0, relpath),
      OP_VIEWAS, unknown op
    - app_handle_ext_key: all Ctrl (f/z/k/`/other), Alt (p/P/a/v/i/s/h/d/b/k/=/Left/Right/other),
      and plain (Enter/other) bindings, incl. right-pane active for diff and OOM dialogs
AE2 qemu-aarch64 matches x86_64 byte for byte
AE3 mutation check (16 mutants)
"""

import subprocess
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRC = "src/app/app_ext.c"
REFS = [
    "tests/harness/app_ext_reference.s",
    "tests/harness/app_ext_keys_reference.s",
]
CORE = [
    "src/core/oop_rt.c", "src/core/string.c", "src/core/string_path.c", "src/core/string_format.c",
    "src/core/utf8.c", "src/core/memory.c", "src/core/mem_prims.c",
    "build/i18n_en.c",
]


def build(arch: str, t: Path, src_ovr: Path | None = None) -> Path:
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
            "--redefine-sym", "FilePane_nav_to=fake_FilePane_nav_to",
            "--redefine-sym", "FilePane_ensure_visible=fake_FilePane_ensure_visible",
            "--redefine-sym", "FilePane_hist_back=fake_FilePane_hist_back",
            "--redefine-sym", "FilePane_hist_fwd=fake_FilePane_hist_fwd",
            "--redefine-sym", "DirModel_find_by_name=fake_DirModel_find_by_name",
            "--redefine-sym", "DirModel_sort=fake_DirModel_sort",
            "--redefine-sym", "dir_search_run=fake_dir_search_run",
            "--redefine-sym", "dir_diff_mark=fake_dir_diff_mark",
            "--redefine-sym", "FuzzyDialog_get_selected=fake_FuzzyDialog_get_selected",
            "--redefine-sym", "BookmarkDialog_get_selected=fake_BookmarkDialog_get_selected",
            "--redefine-sym", "config_menu_count=fake_config_menu_count",
            "--redefine-sym", "config_menu_item=fake_config_menu_item",
            "--redefine-sym", "pkg_menu_get=fake_pkg_menu_get",
        ]
        for o in objs:
            subprocess.run(["objcopy", *redefs, str(o)], check=True, capture_output=True)
        extra += objs
    defs = [f'-DEXT_SRC="{src_ovr or (ROOT / SRC)}"']
    return H.build(arch, t, ["tests/harness/app_ext_cli.c", *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *defs])


MOD_ALT, MOD_CTRL = 2, 4
KEY_LEFT, KEY_RIGHT, KEY_ENTER = 0x100003, 0x100004, 0x10000E

SCRIPT = "\n".join([
    # app_jump_virtual_entry
    "J 0 4 /a/b/file.txt",
    "J 0 2 /rootfile",
    "J 0 1 barename",
    "J 0 -1 /a/b/missing",
    "J -13 4 /a/b/denied",
    # App_close_dialog_ext: D <op_tag> <arg1> <arg2> <arg3>
    "D 12 1 1 1",       # OP_USERMENU cfg item found
    "D 12 0 0 1",       # OP_USERMENU cfg item missing
    "D 12 2 1 1",       # OP_USERMENU exact boundary cur == cnt (pkg idx 0)
    "D 12 3 1 1",       # OP_USERMENU pkg item (idx 1) found
    "D 12 3 1 0",       # OP_USERMENU pkg item NULL
    "D 13 0 0 0",       # OP_BOOKMARKS NULL
    "D 13 1 0 0",       # OP_BOOKMARKS OK
    "D 14 1 -2 0",      # OP_FRECENCY nav -ENOENT
    "D 15 1 0 0",       # OP_CONFLICT
    "D 16 0 1 1",       # OP_SEARCH 0 hits -> msg_search_none
    "D 16 5 1 5",       # OP_SEARCH 5 hits with dotdot -> cursor=1
    "D 16 5 0 5",       # OP_SEARCH 5 hits without dotdot -> cursor=0
    "D 16 1 1 1",       # OP_SEARCH 1 hit with dotdot (count<=1) -> cursor=0
    "D 16 -13 1 1",     # OP_SEARCH error -> no msg_search_none
    "D 9 0 0 0",        # OP_CHMOD
    "D 18 0 0 0",       # OP_SYMLINK
    "D 19 0 0 0",       # OP_HARDLINK
    "D 20 0 1 0",       # OP_PALETTE none
    "D 20 7 1 0",       # OP_PALETTE builtin action 7
    "D 20 4098 1 0",    # OP_PALETTE pkg action 2
    "D 20 4098 0 0",    # OP_PALETTE pkg action NULL
    "D 21 0 0 0",       # OP_FUZZY none
    "D 21 1 0 0",       # OP_FUZZY jump
    "D 22 0 0 0",       # OP_VIEWAS
    "D 99 0 0 0",       # unknown
    # app_handle_ext_key: K <key> <mods> <active_right> <aux>
    f"K {ord('f')} {MOD_CTRL} 0 0",
    f"K {ord('f')} {MOD_CTRL} 0 1",     # OOM SearchDialog
    f"K {ord('z')} {MOD_CTRL} 0 0",
    f"K {ord('k')} {MOD_CTRL} 0 0",
    f"K {ord('`')} {MOD_CTRL} 0 0",
    f"K {ord('x')} {MOD_CTRL} 0 0",
    f"K {ord('p')} {MOD_ALT} 0 0",
    f"K {ord('P')} {MOD_ALT} 0 0",
    f"K {ord('a')} {MOD_ALT} 0 0",
    f"K {ord('v')} {MOD_ALT} 0 0",
    f"K {ord('i')} {MOD_ALT} 0 0",
    f"K {ord('s')} {MOD_ALT} 0 0",
    f"K {ord('h')} {MOD_ALT} 0 0",
    f"K {ord('d')} {MOD_ALT} 0 0",
    f"K {ord('b')} {MOD_ALT} 0 0",
    f"K {ord('k')} {MOD_ALT} 0 0",
    f"K {ord('=')} {MOD_ALT} 0 0",
    f"K {ord('=')} {MOD_ALT} 1 0",      # right pane active
    f"K {KEY_LEFT} {MOD_ALT} 0 1",
    f"K {KEY_LEFT} {MOD_ALT} 0 0",
    f"K {KEY_RIGHT} {MOD_ALT} 0 1",
    f"K {KEY_RIGHT} {MOD_ALT} 0 0",
    f"K {ord('q')} {MOD_ALT} 0 0",
    f"K {KEY_ENTER} 0 0 1",
    f"K {KEY_ENTER} 0 0 0",
    f"K {ord('a')} 0 0 0",
]) + "\n"


def test_differential(exe: Path) -> str:
    out = H.diff_impls(exe, stdin=SCRIPT.encode()).decode()
    for needle in ("call FilePane_nav_to p=1 |/a/b|", "call FilePane_nav_to p=1 |/|",
                   "call App_exec_menu_tmpl 1 |echo_cmd|", "call App_run_pkg_action 1 a=1",
                   "DE rc=-2", "DE rc=0 cur=1 scr=0", "call App_dispatch_action 1 id=7",
                   "call FilePane_nav_to p=1 |/base/sub|", "call App_view_as_done 1",
                   "call dir_diff_mark a=1 b=2", "call dir_diff_mark a=2 b=1",
                   "call App_cmd_packages 1", "call arc_handle_enter 1"):
        assert needle in out, f"coverage missing: {needle}"
    print(f"[PASS] AE1. app_ext matches frozen asm byte for byte ({len(out.splitlines())} lines)")
    return out


def test_archive_keys(exe: Path) -> None:
    """Alt+F9 unpacks, Alt+Shift+F9 tests an archive, a confirmed Unpack dialog (OP_UNPACK) starts the unpack with its text (C side only)."""
    key_f9, alt, alt_shift = 0x100019, MOD_ALT, MOD_ALT | 1
    out = H.run_impl("x86_64", exe, "c", stdin=f"K {key_f9} {alt} 0 0\nK {key_f9} {alt_shift} 0 0\nD 23 0 0 0\n".encode()).stdout.decode()
    lines = out.splitlines()
    assert lines[0] == "call App_cmd_unpack 1" and lines[1].startswith("EK rc=1"), lines[:2]
    assert lines[2] == "call App_cmd_arctest 1" and lines[3].startswith("EK rc=1"), lines[2:4]
    assert "call App_unpack_confirm 1 d=|/dest/dir|" in out, out
    print("[PASS] AE1b. Alt+F9 -> unpack, Alt+Shift+F9 -> test archive, OP_UNPACK -> App_unpack_confirm with the dialog text")


def test_mutations(t: Path, baseline: str) -> None:
    src = (ROOT / SRC).read_text()
    muts = [
        ("jv-root", "else if (slash == dir) slash[1] = '\\0';", "else if (slash == dir) slash[0] = '\\0';"),
        ("jv-nav", "if (FilePane_nav_to(pane, dir) < 0) return;", "if (FilePane_nav_to(pane, dir) > 0) return;"),
        ("jv-find", "if (idx >= 0) {", "if (idx > 99) {"),
        ("pal-pkg", "if (sel >= 0x1000u) {", "if (sel > 0x2000u) {"),
        ("pal-sub", "PkgAction *act = pkg_menu_get((int)(sel - 0x1000u));",
         "PkgAction *act = pkg_menu_get((int)(sel - 0x1001u));"),
        ("um-split", "if (cur < cnt) {", "if (cur <= cnt) {"),
        ("bm-err", "intptr_t rc = FilePane_nav_to(pane, sel);\n        if (rc < 0) return rc;",
         "intptr_t rc = FilePane_nav_to(pane, sel);\n        if (rc < -999) return rc;"),
        ("srch-cur", "if (m->has_dotdot && m->count > 1) cur = 1;",
         "if (m->has_dotdot && m->count >= 1) cur = 1;"),
        ("srch-none", "if (hits == 0)", "if (hits <= 0)"),
        ("hrd-flag", "case OP_HARDLINK: App_apply_link(app, (InputDialog *)dlg, 1); break;",
         "case OP_HARDLINK: App_apply_link(app, (InputDialog *)dlg, 0); break;"),
        ("alt-case", "int lc = key | 0x20;", "int lc = key;"),
        ("alt-diff", "if (key == '=') return app_cmd_diff(app);",
         "if (key == '+') return app_cmd_diff(app);"),
        ("diff-other", "FilePane *other = (act == right) ? (FilePane *)(uintptr_t)app->left_pane : right;",
         "FilePane *other = right; (void)act;"),
        ("hist-empty", "if (!ok) {", "if (ok) {"),
        ("ctrl-term", "if (key == '`') { term_toggle(app); App_layout(app); return 1; }",
         "if (key == '~') { term_toggle(app); App_layout(app); return 1; }"),
        ("plain-ent", "if (key == KEY_ENTER) return arc_handle_enter(app);",
         "if (key == KEY_LEFT) return arc_handle_enter(app);"),
    ]
    survivors: list[str] = []

    def check(idx: int, name: str, old: str, new: str) -> None:
        assert old in src and old != new, name
        mp = t / f"m{idx}" / "app_ext.c"
        mp.parent.mkdir(parents=True, exist_ok=True)
        mp.write_text(src.replace(old, new, 1))
        exe = build("x86_64", t / f"mb{idx}", mp)
        r = H.run_impl("x86_64", exe, "c", stdin=SCRIPT.encode())
        if r.returncode == 0 and r.stdout.decode() == baseline:
            survivors.append(name)

    H.parallel([lambda i=i, m=m: check(i, *m) for i, m in enumerate(muts)])
    assert not survivors, f"surviving mutants: {survivors}"
    print(f"[PASS] AE3. {len(muts)}/{len(muts)} mutants killed across app_ext.c")


def main() -> None:
    with H.workdir() as wd:
        t = Path(wd)
        exe = build("x86_64", t / "x86")
        out = test_differential(exe)
        test_archive_keys(exe)
        if H.available("aarch64"):
            exe_arm = build("aarch64", t / "arm64")
            r = H.run_impl("aarch64", exe_arm, "c", stdin=SCRIPT.encode())
            assert r.returncode == 0 and r.stdout.decode() == out, "aarch64 differs"
            print("[PASS] AE2. qemu-aarch64 matches x86_64 byte for byte")
        test_mutations(t, out)


if __name__ == "__main__":
    main()
