#!/usr/bin/env python3
"""A1.3: src/app/app_task.c versus frozen x86_64 assembly oracles (app_task.s + app_task_tick.s).

AT1 differential (x86_64):
    - task_status_errno (-ECANCELED MSG_WARN vs -EIO MSG_ERR)
    - App_task_start: busy (-EBUSY), empty/NULL dst on copy, NULL dst on delete,
      empty cursor, '..' parent entry, single cursor item (dst existing dir / reg /
      ENOENT / EACCES), single marked item (batch join_dst=1), multi-marked items,
      mem_alloc OOM (-ENOMEM), fs_task_start failure, happy start
    - App_task_tick: inactive fast-path, poll finished (COPY/MOVE/DELETE ok,
      ECANCELED, EIO, active OP_PROGRESS freed vs other dialog preserved),
      periodic snapshot threshold (<80ms vs >=80ms), conflict dialog (no dialog,
      replaces OP_PROGRESS, yields to other dialog), delayed ProgressDialog
      auto-open (before deadline, hidden, blocked by dialog, opens at deadline)
    - App_task_show, App_task_hide, App_task_toggle_pause, App_task_cancel,
      App_task_confirm_quit, App_task_shutdown, App_task_conflict_done (OK vs CANCEL)
AT2 qemu-aarch64 matches x86_64 byte for byte
AT3 mutation check (16 mutants across all branches)
"""

import subprocess
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRC = "src/app/app_task.c"
REFS = [
    "tests/harness/app_task_reference.s",
    "tests/harness/app_task_tick_reference.s",
]
CORE = [
    "src/core/oop_rt.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c",
    "src/core/mem_prims.c",
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
            "--redefine-sym", "mem_alloc=fake_mem_alloc",
            "--redefine-sym", "os_statat=fake_os_statat",
            "--redefine-sym", "os_clock_ms=fake_os_clock_ms",
            "--redefine-sym", "fs_task_is_active=fake_fs_task_is_active",
            "--redefine-sym", "fs_task_start=fake_fs_task_start",
            "--redefine-sym", "fs_task_wake_fd=fake_fs_task_wake_fd",
            "--redefine-sym", "fs_task_poll=fake_fs_task_poll",
            "--redefine-sym", "fs_task_snapshot=fake_fs_task_snapshot",
            "--redefine-sym", "fs_task_check_conflict=fake_fs_task_check_conflict",
            "--redefine-sym", "fs_task_toggle_pause=fake_fs_task_toggle_pause",
            "--redefine-sym", "fs_task_cancel=fake_fs_task_cancel",
            "--redefine-sym", "fs_task_shutdown=fake_fs_task_shutdown",
            "--redefine-sym", "fs_task_reply_conflict=fake_fs_task_reply_conflict",
            "--redefine-sym", "input_set_wake_fd=fake_input_set_wake_fd",
            "--redefine-sym", "DirModel_count_selected=fake_DirModel_count_selected",
            "--redefine-sym", "DirModel_get_entry=fake_DirModel_get_entry",
            "--redefine-sym", "DirModel_entry_name=fake_DirModel_entry_name",
            "--redefine-sym", "FilePane_get_selected_entry=fake_FilePane_get_selected_entry",
        ]
        for o in objs:
            subprocess.run(["objcopy", *redefs, str(o)], check=True, capture_output=True)
        extra += objs
    defs = [f'-DTASK_SRC="{src_ovr or (ROOT / SRC)}"']
    return H.build(arch, t, ["tests/harness/app_task_cli.c", *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *defs])


SCRIPT = "\n".join([
    # task_status_errno
    "C 0 -125",   # -ECANCELED -> MSG_WARN
    "C 0 -5",     # -EIO -> MSG_ERR
    # App_task_start: S <active> <op> <sel_mask> <cur_idx> <oom> <stat_rc> <is_dir> <start_rc> <dst>
    "S 1 1 0 1 0 0 1 0 /dst",         # busy -> -EBUSY
    "S 0 1 0 1 0 0 1 0 -",            # COPY NULL dst -> no-op
    "S 0 2 0 1 0 0 1 0 !",            # MOVE empty dst -> no-op
    "S 0 1 0 -1 0 0 1 0 /dst",        # NULL cursor entry -> no-op
    "S 0 1 0 0 0 0 1 0 /dst",         # '..' parent entry -> MSG_WARN
    "S 0 1 0 1 1 0 1 0 /dst",         # mem_alloc OOM -> -ENOMEM
    "S 0 1 0 1 0 -13 0 0 /dst",       # stat EACCES -> error, no start
    "S 0 1 0 1 0 -2 0 0 /dst/new",    # stat ENOENT -> join_dst=0
    "S 0 1 0 1 0 0 0 0 /dst/file",    # stat S_IFREG -> join_dst=0
    "S 0 1 0 1 0 0 1 0 /dst/dir",     # stat S_IFDIR -> join_dst=1
    "S 0 1 2 1 0 -2 0 0 /dst/one",    # 1 marked item -> batch join_dst=1 (no stat)
    "S 0 2 10 1 0 0 0 0 /dst/multi",  # 2 marked items (idx 1,3) -> multi names
    "S 0 3 0 2 0 0 0 0 -",            # DELETE with NULL dst -> starts
    "S 0 1 0 1 0 0 1 -5 /dst/dir",    # fs_task_start returns -EIO
    # App_task_tick: T <active> <poll_done> <poll_res> <now_ms> <last_ms> <dlg_at> <hidden> <cfl> <cur_dlg_tag> <snap_op>
    "T 0 1 0 2000 0 0 0 0 0 1",       # inactive -> fast no-op
    "T 1 1 0 2000 0 0 0 0 10 1",      # done OK COPY, frees OP_PROGRESS
    "T 1 1 0 2000 0 0 0 0 11 2",      # done OK MOVE, keeps OP_QUIT
    "T 1 1 0 2000 0 0 0 0 0 3",       # done OK DELETE
    "T 1 1 -125 2000 0 0 0 0 0 1",    # done ECANCELED
    "T 1 0 0 1079 1000 1300 0 0 0 1",   # elapsed 79 < 80 -> no snapshot, now < dlg_at
    "T 1 0 0 1080 1000 1300 0 0 0 1",   # elapsed 80 >= 80 -> snapshot, now < dlg_at
    "T 1 0 0 1300 1300 1300 1 0 0 1",   # hidden=1 -> no auto ProgressDialog
    "T 1 0 0 1300 1300 1300 0 0 11 1",  # other dialog open -> no auto ProgressDialog
    "T 1 0 0 1300 1300 1300 0 0 0 1",   # now >= dlg_at -> auto ProgressDialog
    "T 1 0 0 1100 1100 1300 0 1 0 1",   # conflict with no dialog -> ConflictDialog
    "T 1 0 0 1100 1100 1300 0 1 10 1",  # conflict replaces OP_PROGRESS
    "T 1 0 0 1100 1100 1300 0 1 11 1",  # conflict yields to OP_QUIT
    # Controls
    "C 1 0 0",    # App_task_show inactive
    "C 1 1 1",    # App_task_show blocked by dialog
    "C 1 1 0",    # App_task_show opens ProgressDialog
    "C 2",        # App_task_hide
    "C 3",        # App_task_toggle_pause + App_task_cancel
    "C 4 1",      # App_task_confirm_quit blocked by dialog
    "C 4 0",      # App_task_confirm_quit opens ConfirmDialog
    "C 5",        # App_task_shutdown
    "C 6 1 3 1",  # App_task_conflict_done DLG_OK (RENAME, for_all=1)
    "C 6 2 1 0",  # App_task_conflict_done DLG_CANCEL (SKIP all + cancel)
]) + "\n"


def test_differential(exe: Path) -> str:
    out = H.diff_impls(exe, stdin=SCRIPT.encode()).decode()
    for needle in ("E_BUSY", "E_NOMEM", "E_ACCES", "E_CANCELED",
                   "join=0 src=|/src/dir| dst=|/dst/new|",
                   "join=1 src=|/src/dir| dst=|/dst/dir|",
                   "names=|one.txt;three.txt;|",
                   "call dlg_dtor tag=10",
                   "call ConflictDialog_new 1 n=|clash.bin| s=1234 d=5678",
                   "call ProgressDialog_new 1",
                   "call ConfirmDialog_new 1",
                   "call fs_task_reply_conflict act=3 all=1",
                   "call fs_task_reply_conflict act=2 all=1"):
        assert needle in out, f"coverage missing: {needle}"
    print(f"[PASS] AT1. app_task matches frozen asm byte for byte ({len(out.splitlines())} lines)")
    return out


def test_mutations(t: Path, baseline: str) -> None:
    src = (ROOT / SRC).read_text()
    muts = [
        ("cancel-warn", "int kind = (neg_errno == -ECANCELED) ? MSG_WARN : MSG_ERR;",
         "int kind = MSG_ERR; (void)neg_errno;"),
        ("busy-chk", "if (fs_task_is_active()) {", "if (0) {"),
        ("del-dst", "if (op != TASK_OP_DELETE && (!dst_input || dst_input[0] == '\\0')) return;",
         "if (!dst_input || dst_input[0] == '\\0') return;"),
        ("parent-chk", "if (fe->flags & FE_IS_PARENT) {", "if (0) {"),
        ("stat-enoent", "if (sr != -ENOENT) {", "if (sr == -ENOENT) {"),
        ("stat-dir", "job->join_dst = ((st.mode & S_IFMT) == S_IFDIR) ? 1u : 0u;",
         "job->join_dst = 0u; (void)st;"),
        ("batch-join", "if (count == 1 && sel_count == 0) {", "if (count == 1 && sel_count <= 1) {"),
        ("dlg-delay", "app->task_dlg_at = (uint64_t)(os_clock_ms() + TASK_DLG_DELAY_MS);",
         "app->task_dlg_at = (uint64_t)os_clock_ms();"),
        ("fin-prog", "if (ad && ad->op_tag == OP_PROGRESS) {", "if (ad) {"),
        ("fin-move", "else if (op == TASK_OP_MOVE) msg = lang->s[LS_app_ok_move];",
         "else if (op == TASK_OP_COPY) msg = lang->s[LS_app_ok_move];"),
        ("tick-80", "if (now - app->task_last_ms >= (uint64_t)(TASK_TICK_MS - 20)) {",
         "if (now - app->task_last_ms > (uint64_t)(TASK_TICK_MS - 20)) {"),
        ("cfl-guard", "if (!ad || ad->op_tag == OP_PROGRESS) {", "if (!ad) {"),
        ("auto-hid", "if (app->active_dialog == 0 && app->task_hidden == 0 && now >= app->task_dlg_at)",
         "if (app->active_dialog == 0 && now >= app->task_dlg_at)"),
        ("cancel-unpause", "snap->paused = 0;", "snap->paused = 1;"),
        ("cfl-ok", "if (result == DLG_OK) {", "if (result != DLG_OK) {"),
        ("cfl-skip-all", "fs_task_reply_conflict(CONFLICT_ACT_SKIP, 1);",
         "fs_task_reply_conflict(CONFLICT_ACT_SKIP, 0);"),
    ]
    survivors: list[str] = []

    def check(idx: int, name: str, old: str, new: str) -> None:
        assert old in src and old != new, name
        mp = t / f"m{idx}" / "app_task.c"
        mp.parent.mkdir(parents=True, exist_ok=True)
        mp.write_text(src.replace(old, new, 1))
        exe = build("x86_64", t / f"mb{idx}", mp)
        r = H.run_impl("x86_64", exe, "c", stdin=SCRIPT.encode())
        if r.returncode == 0 and r.stdout.decode() == baseline:
            survivors.append(name)

    H.parallel([lambda i=i, m=m: check(i, *m) for i, m in enumerate(muts)])
    assert not survivors, f"surviving mutants: {survivors}"
    print(f"[PASS] AT3. {len(muts)}/{len(muts)} mutants killed across app_task.c")


def main() -> None:
    with H.workdir() as wd:
        t = Path(wd)
        exe = build("x86_64", t / "x86")
        out = test_differential(exe)
        if H.available("aarch64"):
            exe_arm = build("aarch64", t / "arm64")
            r = H.run_impl("aarch64", exe_arm, "c", stdin=SCRIPT.encode())
            assert r.returncode == 0 and r.stdout.decode() == out, "aarch64 differs"
            print("[PASS] AT2. qemu-aarch64 matches x86_64 byte for byte")
        test_mutations(t, out)


if __name__ == "__main__":
    main()
