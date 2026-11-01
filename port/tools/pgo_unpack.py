#!/usr/bin/env python3
"""Unpacks pgo_counts.bin (the profile-guided build's training counts, src/plat/pgo.c) into .gcda files.

Each file goes where the training build's compiler named it (the object's path with .gcda), optionally moved:
--from is the start of those paths to replace (default: the common one up to "/build/"), --to the port/ directory of
the tree to build with `make PGO=use` (same BUILD_DIR as the training build).

    port/tools/pgo_unpack.py pgo_counts.bin --to port
"""
import argparse
import os
import struct
import sys


def read_files(data):
    if data[:4] != b"DKRP":
        sys.exit("not a pgo_counts.bin (no DKRP header)")
    pos, files = 4, []
    while pos < len(data):
        (name_len,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = data[pos:pos + name_len].decode()
        pos += name_len
        (size,) = struct.unpack_from("<I", data, pos)
        pos += 4
        files.append((name, data[pos:pos + size]))
        pos += size
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("counts")
    parser.add_argument("--from", dest="src", help="path prefix to replace (default: up to /build/)")
    parser.add_argument("--to", default=None, help="port/ directory to put them under")
    args = parser.parse_args()
    with open(args.counts, "rb") as f:
        files = read_files(f.read())
    for name, gcda in files:
        out = name
        if args.to is not None:
            prefix = args.src if args.src else name[:name.index("/build/") + 1]
            if not name.startswith(prefix):
                sys.exit("%s doesn't start with %s" % (name, prefix))
            out = os.path.join(args.to, name[len(prefix):])
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(gcda)
    print("%d .gcda files written" % len(files))


if __name__ == "__main__":
    main()
