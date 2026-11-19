#!/usr/bin/env python3
"""bin2c.py FILE OUT: OUT.c and OUT.h with the file's bytes as NAME[] and its length as NAME_size, as devkitPro's bin2s
names them (NAME is OUT's last path part), for the backend replay's shader."""
import os
import sys

data = open(sys.argv[1], "rb").read()
out = sys.argv[2]
name = os.path.basename(out)
with open(out + ".h", "w") as h:
    h.write("#pragma once\n#include <stdint.h>\n")
    h.write("extern const uint8_t %s[];\nextern const uint32_t %s_size;\n" % (name, name))
with open(out + ".c", "w") as c:
    c.write("#include <stdint.h>\n")
    c.write("__attribute__((aligned(4))) const uint8_t %s[%d] = {\n" % (name, len(data) + 1))
    for i in range(0, len(data), 16):
        c.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    c.write("};\n")
    c.write("const uint32_t %s_size = %d;\n" % (name, len(data)))
