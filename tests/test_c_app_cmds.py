#!/usr/bin/env python3
"""A2.2: src/app/app_cmds.c versus frozen x86_64 assembly oracle.

AC1 differential (x86_64):
    - App_cmd_newfile, App_cmd_start_filter, App_cmd_goto
    - App_cmd_rename (NULL, FE_IS_PARENT, normal)
    - App_cmd_view (NULL, FE_IS_DIR, FE_LINK_DIR, archive virtual_mode=2 OK/fail,
      VFS virtual_mode=3 OK/fail, normal file)
    - App_cmd_edit (NULL, FE_IS_DIR, normal file)
    - App_cmd_shell (chdir to active dir, exec shell, restore cwd, force_full + reload)
    - App_cmd_copy / App_cmd_move (selected count > 0, NULL cursor, FE_IS_PARENT, normal)
    - App_cmd_delete (NULL cursor, FE_IS_PARENT, single item, multi-selected count)
AC2 asm defect proof: open_transfer_dlg (app_cmds.s:282-284) popped rbx then r12
    (swapping caller's callee-saved rbx/r12); C preserves both
AC3 qemu-aarch64 matches x86_64 byte for byte
AC4 mutation check (14 mutants)
"""

import subprocess
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRC = "src/app/app_cmds.c"
REF = "tests/harness/app_cmds_reference.s"
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
        o = t / "app_cmds_reference.o"
        subprocess.run(["as", *inc, str(ROOT / REF), "-o", str(o)], check=True, capture_output=True)
        H.rename_defined_globals([o], "ref_")
        redefs = [
            "--redefine-sym", "FilePane_get_selected_entry=fake_FilePane_get_selected_entry",
            "--redefine-sym", "DirModel_entry_name=fake_DirModel_entry_name",
            "--redefine-sym", "DirModel_count_selected=fake_DirModel_count_selected",
            "--redefine-sym", "arc_extract_selected=fake_arc_extract_selected",
            "--redefine-sym", "pkg_vfs_read_uri=fake_pkg_vfs_read_uri",
            "--redefine-sym", "sys_exec_sh_arg=fake_sys_exec_sh_arg",
        ]
        subprocess.run(["objcopy", *redefs, str(o)], check=True, capture_output=True)
        extra.append(o)
    defs = [f'-DCMDS_SRC="{src_ovr or (ROOT / SRC)}"']
    return H.build(arch, t, ["tests/harness/app_cmds_cli.c", *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *defs])


def build_script(work: Path) -> str:
    d_act = work / "act"
    d_act.mkdir(exist_ok=True)
    return "\n".join([
        "N",
        "F",
        "G",
        "R 0 0", "R 1 32", "R 1 0",
        "V 0 0 0 0 0",     # NULL
        "V 1 1 0 0 0",     # FE_IS_DIR
        "V 1 64 0 0 0",    # FE_LINK_DIR
        "V 1 0 2 0 0",     # archive OK
        "V 1 0 3 0 0",     # VFS OK
        "V 1 0 3 0 -1",    # VFS fail
        "V 1 0 0 0 0",     # normal file
        "E 0 0", "E 1 1", "E 1 0",
        f"S {d_act}",
        "C 0 0 0", "C 0 1 32", "C 0 1 0", "C 3 0 0",
        "M 0 0 0", "M 0 1 32", "M 0 1 0", "M 2 0 0",
        "D 0 0 0", "D 0 1 32", "D 0 1 0", "D 5 0 0",
    ]) + "\n"


def test_differential(exe: Path, t: Path) -> str:
    s = build_script(t / "w").encode()
    out = H.diff_impls(exe, stdin=s).decode()
    for needle in ("tag=6", "i=|*.c| tag=7", "i=|/act/dir| tag=8", "i=|hello.txt| tag=2",
                   "call App_open_viewer 1 p=|/tmp/arc_item|",
                   "call App_open_viewer 1 p=|/tmp/vfs_item|",
                   "call App_open_viewer 1 p=|/act/dir/hello.txt|",
                   "a1=|/act/dir/hello.txt|", "SH cwd=1 ST d=0 rd=0 ff=1",
                   "i=|/other/dir| tag=3", "i=|/other/dir| tag=4",
                   "d=|hello.txt| tag=5", "d=|5 "):
        assert needle in out, f"coverage missing: {needle}"
    print(f"[PASS] AC1. app_cmds matches frozen asm byte for byte ({len(out.splitlines())} lines)")
    return out


def test_archive_member_errors(exe: Path) -> None:
    """F3 on an archive member that cannot be read says why (the frozen asm returned silently): encrypted / unsupported method
    (-EACCES, -EOPNOTSUPP) and damaged data (anything else); the viewer is not opened; success still opens it."""
    script = b"".join(f"V 1 0 2 {rc} 0\n".encode() for rc in (-13, -95, -74, -22, -5, 0))
    out = H.run_impl("x86_64", exe, "c", stdin=script).stdout.decode()
    unsup, bad = "encrypted or uses an unsupported compression method", "its data is damaged or fails the checksum"
    msgs = [ln for ln in out.splitlines() if ln.startswith("call App_set_status")]
    assert [unsup in m for m in msgs] == [True, True, False, False, False] and all(bad in m for m in msgs[2:]), msgs
    assert out.count("call App_open_viewer 1 p=|/tmp/arc_item|") == 1 and out.count("call App_open_viewer") == 1, out
    print("[PASS] AC1b. an unreadable archive member reports 'unsupported / encrypted' or 'damaged', and the viewer stays closed")


def test_archive_pane_commands(exe: Path) -> None:
    """In an archive pane F5 hands over to the unpack command; F6 and F8 are refused with the read-only message (C side only)."""
    out = H.run_impl("x86_64", exe, "c", stdin=b"A 0\nA 1\nA 2\n").stdout.decode()
    assert out.count("call App_cmd_unpack") == 1, out
    assert out.count("Archive is read-only") == 2, out
    assert "call App_set_status" in out and "call InputDialog_new" not in out, out
    print("[PASS] AC1c. F5 in an archive pane is the unpack command; F6 / F8 there are refused as read-only")


def test_asm_abi_bug(exe: Path) -> None:
    s = b"B 0\nB 1\n"
    r_ref = H.run_impl("x86_64", exe, "r", stdin=s).stdout.decode()
    r_c = H.run_impl("x86_64", exe, "c", stdin=s).stdout.decode()
    assert r_ref.count("ABI_CLOBBER=1") == 2, f"expected asm oracle to swap rbx/r12: {r_ref}"
    assert r_c.count("ABI_CLOBBER=0") == 2, f"C must preserve rbx/r12: {r_c}"
    print("[PASS] AC2. asm oracle swaps callee-saved rbx/r12 in open_transfer_dlg; C preserves ABI")


def test_mutations(t: Path, baseline: str) -> None:
    s = build_script(t / "w").encode()
    src = (ROOT / SRC).read_text()
    muts = [
        ("nf-tag", "OP_NEWFILE);", "OP_MKDIR);"),
        ("flt-init", "m->filter, OP_FILTER", "m->path, OP_FILTER"),
        ("ren-par", "if (fe->flags & FE_IS_PARENT) {", "if (0) {"),
        ("vw-dir", "if (fe->flags & (FE_IS_DIR | FE_LINK_DIR)) {", "if (fe->flags & FE_IS_DIR) {"),
        ("vw-arc", "if (m->virtual_mode == 2) {", "if (m->virtual_mode == 9) {"),
        ("vw-vfs", "if (m->virtual_mode == 3) {", "if (m->virtual_mode == 9) {"),
        ("ed-ff", "scr->force_full = 1;", "scr->force_full = 0;"),
        ("sh-cmd", '"exec \\"${SHELL:-/bin/sh}\\""', '"exec /bin/false"'),
        ("tr-mv", "int is_mv = (op_tag == OP_MOVE);", "int is_mv = (op_tag == OP_COPY);"),
        ("tr-sel", "if (DirModel_count_selected(m, nullptr) == 0) {",
         "if (DirModel_count_selected(m, nullptr) != 0) {"),
        ("del-cnt", "size_t cnt = DirModel_count_selected(m, nullptr);",
         "size_t cnt = 0; (void)DirModel_count_selected(m, nullptr);"),
        ("del-sfx", "(void)str_cat(buf, lang->s[LS_app_sel_items_sfx], sizeof buf);", ""),
        ("del-one", "lang->s[LS_app_msg_del_one]", "lang->s[LS_app_msg_del_many]"),
        ("del-par", "lang->s[LS_app_err_parent], MSG_WARN);", "lang->s[LS_app_err_isdir], MSG_WARN);"),
    ]
    survivors: list[str] = []

    def check(idx: int, name: str, old: str, new: str) -> None:
        assert old in src and old != new, name
        mp = t / f"m{idx}" / "app_cmds.c"
        mp.parent.mkdir(parents=True, exist_ok=True)
        mp.write_text(src.replace(old, new, 1))
        exe = build("x86_64", t / f"mb{idx}", mp)
        r = H.run_impl("x86_64", exe, "c", stdin=s)
        if r.returncode == 0 and r.stdout.decode() == baseline:
            survivors.append(name)

    H.parallel([lambda i=i, m=m: check(i, *m) for i, m in enumerate(muts)])
    assert not survivors, f"surviving mutants: {survivors}"
    print(f"[PASS] AC4. {len(muts)}/{len(muts)} mutants killed across app_cmds.c")


def main() -> None:
    with H.workdir() as wd:
        t = Path(wd)
        (t / "w").mkdir()
        exe = build("x86_64", t / "x86")
        out = test_differential(exe, t)
        test_archive_member_errors(exe)
        test_archive_pane_commands(exe)
        test_asm_abi_bug(exe)
        if H.available("aarch64"):
            exe_arm = build("aarch64", t / "arm64")
            r = H.run_impl("aarch64", exe_arm, "c", stdin=build_script(t / "w").encode())
            assert r.returncode == 0 and r.stdout.decode() == out, "aarch64 differs"
            print("[PASS] AC3. qemu-aarch64 matches x86_64 byte for byte")
        test_mutations(t, out)


if __name__ == "__main__":
    main()
