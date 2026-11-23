#!/usr/bin/env python3
"""Deterministic archive corpus shared by the archive tests (docs/ARCHIVE_TOOLS_PLAN.md, every phase).

build_corpus(root) writes one archive per case and returns {name: Fixture}; a Fixture carries the entries the archive must show
and unpack to, so a test compares Duop with the data instead of with another reader. Everything is seeded: the same call gives the
same bytes. Formats Python cannot write (tar.zst, .zst) need the `zstd` tool and are skipped when it is missing.

    python3 tests/archive_fixtures.py [dir]      writes the corpus and cross-checks it with tarfile / zipfile / bz2 / lzma
"""

import bz2
import gzip
import hashlib
import io
import lzma
import os
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

MTIME = 1700000000


@dataclass(frozen=True)
class Expect:
    name: str
    kind: str = "file"          # file | dir | symlink | hardlink
    data: bytes = b""
    mode: int = 0o644
    link: str = ""


@dataclass
class Fixture:
    path: Path
    fmt: str                    # tar tgz tbz2 txz tzst zip gz bz2 xz zst
    entries: list[Expect] = field(default_factory=list)
    note: str = ""


def payload(seed: int, size: int) -> bytes:
    rng = random.Random(seed)
    words = [rng.randbytes(rng.randrange(3, 9)) for _ in range(40)]
    out = bytearray()
    while len(out) < size:                                   # compressible but not trivial
        out += rng.choice(words) if rng.random() < 0.8 else rng.randbytes(rng.randrange(1, 20))
    return bytes(out[:size])


def tree() -> list[Expect]:
    """The tree most fixtures hold: names, sizes and kinds that stress path handling (not the symlink / hardlink cases)."""
    return [
        Expect("readme.txt", data=b"hello archive\n"),
        Expect("empty.bin"),
        Expect("bin/tool", data=payload(1, 3000), mode=0o755),
        Expect("docs/", "dir", mode=0o755),
        Expect("docs/guide/", "dir", mode=0o755),
        Expect("docs/guide/ch1.md", data=payload(2, 70000)),
        Expect("blk512.dat", data=payload(3, 512)),
        Expect("blk513.dat", data=payload(4, 513)),
        Expect("ünï/é.txt", data="unicode ✓\n".encode()),
        Expect("emptydir/", "dir", mode=0o755),
        Expect("long/" + "d" * 60 + "/" + "f" * 90 + ".txt", data=b"long path\n"),
        Expect("n" * 150 + ".txt", data=b"long name\n"),
        Expect("deep/" + "/".join(f"l{i}" for i in range(12)) + "/leaf", data=b"deep\n"),
    ]


def links() -> list[Expect]:
    return [Expect("real.txt", data=b"target\n"), Expect("sym", "symlink", link="real.txt"),
            Expect("hard", "hardlink", link="real.txt"), Expect("dsym", "symlink", link="docs")]


def _tar_member(e: Expect, fmt: int) -> tuple[tarfile.TarInfo, io.BytesIO | None]:
    ti = tarfile.TarInfo(e.name.rstrip("/"))
    ti.mtime, ti.mode = MTIME, e.mode
    if e.kind == "dir":
        ti.type, ti.mode = tarfile.DIRTYPE, e.mode or 0o755
        return ti, None
    if e.kind in ("symlink", "hardlink"):
        ti.type = tarfile.SYMTYPE if e.kind == "symlink" else tarfile.LNKTYPE
        ti.linkname = e.link
        return ti, None
    ti.size = len(e.data)
    return ti, io.BytesIO(e.data)


def tar_bytes(entries: list[Expect], fmt: int = tarfile.PAX_FORMAT) -> bytes:
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=fmt) as tf:
        for e in entries:
            ti, fo = _tar_member(e, fmt)
            tf.addfile(ti, fo)
    return bio.getvalue()


def zip_bytes(entries: list[Expect], method: int = zipfile.ZIP_DEFLATED, force64: bool = False, comment: bytes = b"") -> bytes:
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w") as zf:
        for e in entries:
            zi = zipfile.ZipInfo(e.name, date_time=(2023, 11, 14, 22, 13, 20))
            zi.compress_type = method
            if e.kind == "dir":
                zi.external_attr = (0o40000 | e.mode) << 16 | 0x10
                zf.writestr(zi, b"")
            elif e.kind == "symlink":
                zi.external_attr = (0o120777) << 16
                zf.writestr(zi, e.link.encode())
            else:
                zi.external_attr = (0o100000 | e.mode) << 16
                with zf.open(zi, "w", force_zip64=force64) as w:
                    w.write(e.data)
        zf.comment = comment
    return bio.getvalue()


def many_entries(n: int) -> list[Expect]:
    return [Expect(f"d{i % 97:02d}/s{i % 11}/f{i:06d}.txt", data=f"entry {i}\n".encode()) for i in range(n)]


def tar_base256(entries: list[Expect]) -> bytes:
    """A ustar archive whose size fields use the GNU base-256 form (high bit set): legal for any size, here for small files."""
    out = bytearray()
    for e in entries:
        if e.kind != "file":
            continue
        h = bytearray(512)
        h[0:len(e.name)] = e.name.encode()
        h[100:108] = b"0000644\0"
        h[108:124] = b"0000000\0" * 2
        h[124:136] = b"\x80" + len(e.data).to_bytes(11, "big")
        h[136:148] = b"%011o\0" % MTIME
        h[148:156] = b" " * 8
        h[156] = ord("0")
        h[257:263] = b"ustar\0"
        h[263:265] = b"00"
        h[148:156] = b"%06o\0 " % sum(h)
        out += h + e.data + b"\0" * (-len(e.data) % 512)
    return bytes(out) + b"\0" * 1024


def zip_cp437(entries: list[Expect]) -> bytes:
    """Names without the UTF-8 flag: 'cafX' is patched to CP437 'café' (0x82) in the local and the central header."""
    raw = zip_bytes([Expect("cafX.txt", data=b"cp437 name\n")], zipfile.ZIP_STORED)
    return raw.replace(b"cafX.txt", b"caf\x82.txt")


def zip_with_prefix(entries: list[Expect], prefix: bytes) -> bytes:
    """A self-extractor style stub before an *unadjusted* zip: every offset is relative to where the zip starts."""
    return prefix + zip_bytes(entries)


def zip_encrypted_flag(entries: list[Expect]) -> bytes:
    z = bytearray(zip_bytes(entries, zipfile.ZIP_STORED))
    for sig, off in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        i = 0
        while (i := z.find(sig, i)) >= 0:
            z[i + off] |= 1
            i += 4
    return bytes(z)


def zip_overlap(entries: list[Expect]) -> bytes:
    """Two central records pointing at the same local header (the first one duplicated): the input of an overlapping-entry bomb."""
    z = zip_bytes(entries[:1], zipfile.ZIP_STORED)
    c = z.index(b"PK\x01\x02")
    e = z.rindex(b"PK\x05\x06")
    rec = z[c:e]
    cd_size, cd_off = struct.unpack_from("<II", z, e + 12)
    eocd = bytearray(z[e:])
    struct.pack_into("<HH", eocd, 8, 2, 2)
    struct.pack_into("<I", eocd, 12, cd_size * 2)
    return z[:e] + rec + bytes(eocd)


def _zstd(data: bytes) -> bytes | None:
    if not shutil.which("zstd"):
        return None
    return subprocess.run(["zstd", "-q", "-c", "-19"], input=data, capture_output=True, check=True).stdout


def build_corpus(root: Path, many: int = 0, big: bool = False) -> dict[str, Fixture]:
    """Write the corpus under root. many=N adds an N-entry tar and zip; big=True a 12 MiB file in a tar.gz and a zip."""
    root.mkdir(parents=True, exist_ok=True)
    out: dict[str, Fixture] = {}

    def put(name: str, fmt: str, data: bytes | None, entries: list[Expect], note: str = "") -> None:
        if data is not None:
            (root / name).write_bytes(data)
            out[name] = Fixture(root / name, fmt, entries, note)

    t = tree()
    tl = t + links()
    plain = tar_bytes(t)
    put("tree.tar", "tar", plain, t)
    put("tree_gnu.tar", "tar", tar_bytes(t, tarfile.GNU_FORMAT), t, "GNU ././@LongLink names")
    put("tree_ustar.tar", "tar", tar_bytes([x for x in t if len(x.name) < 100], tarfile.USTAR_FORMAT), [x for x in t if len(x.name) < 100])
    put("links.tar", "tar", tar_bytes(links()), links())
    put("tree.tar.gz", "tgz", gzip.compress(plain, mtime=0), t)
    put("tree.tgz", "tgz", gzip.compress(tar_bytes(tl), mtime=0), tl)
    put("tree.tar.bz2", "tbz2", bz2.compress(plain), t)
    put("tree.tar.xz", "txz", lzma.compress(plain), t)
    put("tree.tar.zst", "tzst", _zstd(plain), t)
    base256 = [Expect("b256.bin", data=payload(9, 1234)), Expect("b256b.bin", data=b"x")]
    put("base256.tar", "tar", tar_base256(base256), base256, "size fields in base-256")
    nofiles = [x for x in t if x.kind == "file"]
    put("tree.zip", "zip", zip_bytes(t), t)
    put("tree_stored.zip", "zip", zip_bytes(t, zipfile.ZIP_STORED), t)
    put("tree_zip64.zip", "zip", zip_bytes(nofiles, force64=True), nofiles, "ZIP64 records and extra fields")
    put("tree_bzip2.zip", "zip", zip_bytes(nofiles, zipfile.ZIP_BZIP2), nofiles, "method 12")
    put("tree_lzma.zip", "zip", zip_bytes(nofiles, zipfile.ZIP_LZMA), nofiles, "method 14")
    put("links.zip", "zip", zip_bytes(links()), links(), "symlink via unix mode bits")
    put("comment.zip", "zip", zip_bytes(t, comment=b"c" * 300), t)
    put("cp437.zip", "zip", zip_cp437(t), [Expect("café.txt", data=b"cp437 name\n")], "no UTF-8 flag")
    put("sfx.zip", "zip", zip_with_prefix(t, b"MZ" + b"\x90" * 4000), t, "stub before an unadjusted zip")
    put("encrypted.zip", "zip", zip_encrypted_flag(t[:2]), [], "bit 0 set: must be reported as encrypted")
    put("overlap.zip", "zip", zip_overlap([Expect("a.bin", data=b"x" * 200)]), [], "two records, one local header")
    put("empty.zip", "zip", zip_bytes([]), [])
    put("empty.tar", "tar", b"\0" * 10240, [])
    blob = payload(11, 300000)
    put("blob.gz", "gz", gzip.compress(blob, mtime=0), [Expect("blob", data=blob)])
    put("blob.bz2", "bz2", bz2.compress(blob), [Expect("blob", data=blob)])
    put("blob.xz", "xz", lzma.compress(blob), [Expect("blob", data=blob)])
    put("blob.zst", "zst", _zstd(blob), [Expect("blob", data=blob)])
    if many:
        m = many_entries(many)
        put(f"many{many}.tar", "tar", tar_bytes(m), m)
        put(f"many{many}.zip", "zip", zip_bytes(m, zipfile.ZIP_STORED), m)
    if big:
        b = [Expect("big.bin", data=payload(12, 12 * 1024 * 1024)), Expect("small.txt", data=b"after the big one\n")]
        put("big.tar.gz", "tgz", gzip.compress(tar_bytes(b), mtime=0), b)
        put("big.zip", "zip", zip_bytes(b), b)
    return out


def expected_tree(entries: list[Expect]) -> dict[str, tuple]:
    """What an unpack must leave behind: {relpath: (kind, sha256-or-link, mode & 0o777)} for files, directories and symlinks."""
    exp: dict[str, tuple] = {}
    for e in entries:
        p = e.name.rstrip("/")
        if e.kind == "dir":
            exp[p] = ("dir", "", e.mode)
        elif e.kind == "symlink":
            exp[p] = ("symlink", e.link, 0)
        else:
            exp[p] = ("file", hashlib.sha256(e.data if e.kind == "file" else b"").hexdigest(), e.mode)
    return exp


def actual_tree(root: Path) -> dict[str, tuple]:
    """The same digest of a directory on disk (parents of files appear as directories too)."""
    act: dict[str, tuple] = {}
    for dp, dns, fns in os.walk(root, followlinks=False):
        for n in dns + fns:
            p = Path(dp) / n
            rel = p.relative_to(root).as_posix()
            st = p.lstat()
            if p.is_symlink():
                act[rel] = ("symlink", os.readlink(p), 0)
            elif p.is_dir():
                act[rel] = ("dir", "", st.st_mode & 0o777)
            else:
                act[rel] = ("file", hashlib.sha256(p.read_bytes()).hexdigest(), st.st_mode & 0o777)
    return act


def selftest(fx: dict[str, Fixture]) -> None:
    """The generator itself is checked with the standard readers, so a wrong fixture can never excuse a wrong Duop."""
    for name, f in fx.items():
        raw = f.path.read_bytes()
        if f.fmt in ("tar", "tgz", "tbz2", "txz"):
            with tarfile.open(f.path) as tf:
                names = {m.name for m in tf.getmembers() if not m.name.startswith("././")}
            assert names == {e.name.rstrip("/") for e in f.entries}, (name, names)
        elif f.fmt == "zip" and name not in ("encrypted.zip", "overlap.zip", "cp437.zip"):
            with zipfile.ZipFile(f.path) as zf:
                assert {i.filename for i in zf.infolist()} == {e.name for e in f.entries}, name
                assert zf.testzip() is None, name
        elif f.fmt == "gz":
            assert gzip.decompress(raw) == f.entries[0].data
        elif f.fmt == "bz2":
            assert bz2.decompress(raw) == f.entries[0].data
        elif f.fmt == "xz":
            assert lzma.decompress(raw) == f.entries[0].data


if __name__ == "__main__":
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="arcfx_"))
    fixtures = build_corpus(dest, many=2000)
    selftest(fixtures)
    print(f"[PASS] {len(fixtures)} fixtures in {dest}, cross-checked with tarfile / zipfile / gzip / bz2 / lzma")
