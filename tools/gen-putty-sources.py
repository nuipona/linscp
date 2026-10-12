#!/usr/bin/env python3
"""Regenerate source/linux/putty-core.mk from the Windows build projects.

The native Linux build links the same PuTTY core sources as the Windows
build (as listed by the <CppCompile> items of source/*.cbproj), except that
the Windows platform layer (putty/windows/**) is replaced by the POSIX
platform layer vendored in libs/putty/unix/.
"""
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "source", "linux", "putty-core.mk")

files = set()
for path in glob.glob(os.path.join(ROOT, "source", "*.cbproj")):
    text = open(path, encoding="utf-8", errors="replace").read()
    for m in re.finditer(r'<CppCompile Include="([^"]*putty[^"]*)"', text, re.I):
        files.add(m.group(1).replace("\\", "/"))

core = sorted(
    f[len("putty/"):] for f in files
    if f.startswith("putty/") and not f.startswith("putty/windows/")
)

lines = [
    "# Generated from source/*.cbproj by tools/gen-putty-sources.py -- do not edit.",
    "# PuTTY core sources shared with the Windows build (all platform layers",
    "# except putty/windows/**, which the POSIX build replaces with libs/putty/unix).",
    "PUTTY_CORE_SRCS := \\",
]
for i, name in enumerate(core):
    cont = " \\" if i < len(core) - 1 else ""
    lines.append("\t../../source/putty/%s%s" % (name, cont))
open(OUT, "w", newline="\n").write("\n".join(lines) + "\n")
print("wrote %s (%d sources)" % (OUT, len(core)))
