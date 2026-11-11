#!/usr/bin/env python3
"""zip_reader_test.py [out folder]: the archive reader (src/core/zip.cpp) against damaged and hostile archives.

    py tests/zip_reader_test.py        -> build-test/zip_cases/*.zip + cases.txt, build-test/zip_reader_test(.exe), run

Writes the test archives itself (below; nothing is downloaded or copied from a game), builds
tests/zip_reader_test.cpp with src/core/zip.cpp and third_party/zlib, and runs it over them. Each archive is the same
small valid one (a stored a.txt, a deflated b.lua) with one thing wrong; the list says what the reader must answer.
The exit code is the test's (0 = ok), or 2 when the build failed. `--cases-only <folder>` writes the archives and
stops (tests/zip_malformed_extra_field.zip in the repository is the case "extra_field_overrun" from here).
"""
import os, struct, subprocess, sys, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
A_TEXT = b"a stored file of the test of the archive reader\n" * 3
B_TEXT = b"".join(b"-- line %d of a deflated file of the test of the archive reader\n" % i for i in range(60))


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def archive(central=None, eocd=None, extra=None, local_sig=None, corrupt_data=False, zip64=None, cut=None,
            name_len=None):
    """The valid archive with overrides. central: {entry index: {field: value}} for csize / usize / offset; eocd:
    {count / cd_size / cd_offset}; extra: {entry index: bytes} (the extra field of the central header); local_sig:
    {entry index: signature}; corrupt_data: the packed bytes of b.lua damaged; zip64: (count, cd_size, cd_offset) in
    a Zip64 end record with its locator; cut: keep only that many bytes; name_len: {entry index: claimed name
    length}."""
    central, eocd, extra, local_sig, name_len = central or {}, eocd or {}, extra or {}, local_sig or {}, name_len or {}
    files = [(b"a.txt", 0, A_TEXT, A_TEXT), (b"b.lua", 8, deflate(B_TEXT), B_TEXT)]
    out = b""
    heads = []
    for i, (name, method, packed, plain) in enumerate(files):
        if corrupt_data and method == 8:
            packed = packed[:8] + bytes(b ^ 0x5A for b in packed[8:40]) + packed[40:]
        crc = zlib.crc32(plain) & 0xFFFFFFFF
        heads.append((name, method, crc, len(packed), len(plain), len(out)))
        out += struct.pack("<IHHHHHIIIHH", local_sig.get(i, 0x04034B50), 20, 0, method, 0, 0x21, crc, len(packed),
                           len(plain), len(name), 0) + name + packed
    cd_offset = len(out)
    cd = b""
    for i, (name, method, crc, csize, usize, offset) in enumerate(heads):
        o = central.get(i, {})
        x = extra.get(i, b"")
        cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, method, 0, 0x21, crc, o.get("csize", csize),
                          o.get("usize", usize), name_len.get(i, len(name)), len(x), 0, 0, 0, 0,
                          o.get("offset", offset)) + name + x
    out += cd
    if zip64:
        z64_offset = len(out)
        out += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, zip64[0], zip64[0], zip64[1], zip64[2])
        out += struct.pack("<IIQI", 0x07064B50, 0, z64_offset, 1)
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, eocd.get("count", len(heads)), eocd.get("count", len(heads)),
                       eocd.get("cd_size", len(cd)), eocd.get("cd_offset", cd_offset), 0)
    return out[:cut] if cut is not None else out


def cases():
    a, b = len(A_TEXT), len(B_TEXT)
    packed_b = len(deflate(B_TEXT))
    whole = len(archive())
    ok = "open ok 2|a.txt=%d|b.lua=%d" % (a, b)
    # an extra field of the Zip64 kind whose length runs 64 KB past the extra area of the central header
    overrun = struct.pack("<HH", 0x0001, 0xFFFF) + b"\x11" * 4
    return [
        ("valid", archive(), ok),
        ("empty_file", b"", "open fail"),
        ("zeros_22", b"\0" * 22, "open fail"),
        ("no_entries", struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0, 0, 0, 0, 0), "open ok 0"),
        ("cut_in_data", archive(cut=60), "open fail"),
        ("cut_before_end_record", archive(cut=whole - 10), "open fail"),
        ("directory_size_4gb", archive(eocd={"cd_size": 0xFFFFFFF0}), "open fail"),
        ("directory_past_end", archive(eocd={"cd_offset": whole + 100}), "open fail"),
        ("directory_offset_4gb", archive(eocd={"cd_offset": 0xFFFFFF00}), "open fail"),
        ("count_65535", archive(eocd={"count": 0xFFFF}), "open fail"),
        ("zip64_sizes_2_60", archive(zip64=(2, 1 << 60, 0)), "open fail"),
        ("zip64_count_2_40", archive(zip64=(1 << 40, 92, 0)), "open fail"),
        ("extra_field_overrun", archive(central={1: {"usize": 0xFFFFFFFF}}, extra={1: overrun}),
         "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("packed_size_4gb", archive(central={1: {"csize": 0xFFFFFF00}}), "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("stored_size_4gb", archive(central={0: {"csize": 0xFFFFFF00, "usize": 0xFFFFFF00}}),
         "open ok 2|a.txt=fail|b.lua=%d" % b),
        ("unpacked_size_4gb", archive(central={1: {"usize": 0xF0000000}}), "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("unpacked_size_beyond_deflate", archive(central={1: {"usize": (packed_b + 2) * 1032 + 1032}}),
         "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("local_header_past_end", archive(central={1: {"offset": whole + 1000}}), "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("local_header_bad_signature", archive(local_sig={1: 0x12345678}), "open ok 2|a.txt=%d|b.lua=fail" % a),
        ("packed_bytes_damaged", archive(corrupt_data=True), "open ok 2|a.txt=%d|b.lua=fail" % a),
        # a claimed size a little above the real one: the reader answers the bytes there are
        ("unpacked_size_larger", archive(central={1: {"usize": b + 500}}), "open ok 2|a.txt=%d|b.lua=%d" % (a, b)),
        # ... and below: the reader answers the claimed count (the buffer is full; as before the bounds)
        ("unpacked_size_smaller", archive(central={1: {"usize": b - 500}}), "open ok 2|a.txt=%d|b.lua=%d" % (a, b - 500)),
        # a name longer than the directory: the walk ends at that entry
        ("name_past_directory", archive(name_len={1: 0xFFF0}), "open ok 1|a.txt=%d" % a),
    ]


def write_cases(folder):
    os.makedirs(folder, exist_ok=True)
    lines = ["# written by tests/zip_reader_test.py: <archive>|<open>|<entry>=<bytes or fail>..."]
    for name, data, want in cases():
        open(os.path.join(folder, name + ".zip"), "wb").write(data)
        lines.append("%s.zip|%s" % (name, want))
    open(os.path.join(folder, "cases.txt"), "w", newline="\n").write("\n".join(lines) + "\n")
    return os.path.join(folder, "cases.txt")


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--cases-only":
        print(write_cases(sys.argv[2]))
        return 0
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-test")
    obj = os.path.join(out, "obj-zip-reader")
    os.makedirs(obj, exist_ok=True)
    listing = write_cases(os.path.join(out, "zip_cases"))
    # the committed sample must be the one this file writes
    sample = os.path.join(ROOT, "tests", "zip_malformed_extra_field.zip")
    if os.path.exists(sample) and open(sample, "rb").read() != dict((n, d) for n, d, _ in cases())["extra_field_overrun"]:
        print("zip reader test FAILED: tests/zip_malformed_extra_field.zip is not the case extra_field_overrun")
        return 1
    target = ["-target", "x86_64-windows-gnu"] if os.name == "nt" else []
    common = ["-O2", "-mcpu=x86_64_v2", "-ffp-contract=off", "-fno-fast-math", "-g"]
    zlib_inc = ["-I", os.path.join(ROOT, "third_party", "zlib")]
    objs = []
    for n in ("adler32", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees", "zutil"):
        o = os.path.join(obj, n + ".o")
        if subprocess.run(zig("cc", *target, *common, "-std=c99", "-w", *zlib_inc, "-c",
                              os.path.join(ROOT, "third_party", "zlib", n + ".c"), "-o", o)).returncode:
            return 2
        objs.append(o)
    exe = os.path.join(out, "zip_reader_test" + (".exe" if os.name == "nt" else ""))
    if subprocess.run(zig("c++", *target, *common, "-std=c++20", "-I", os.path.join(ROOT, "src"), *zlib_inc,
                          os.path.join(ROOT, "tests", "zip_reader_test.cpp"), os.path.join(ROOT, "src", "core", "zip.cpp"),
                          *objs, "-o", exe, *(["-static"] if os.name == "nt" else []))).returncode:
        return 2
    return subprocess.run([exe, listing]).returncode


if __name__ == "__main__":
    sys.exit(main())
