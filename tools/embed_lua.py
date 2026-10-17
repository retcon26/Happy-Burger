#!/usr/bin/env python3
"""Builds psp/src/lua_builtin.h from psp/lua: the game's own Lua (init.lua and its gamemodes) as C strings,
so they're inside the EBOOT. The Makefile runs it whenever a .lua file changes; the header is kept in git
too, so the dedicated server (which builds from the engine's sources) doesn't need Python.

  python3 tools/embed_lua.py psp/lua psp/src/lua_builtin.h
"""
import os
import sys


def c_string(text):
    out = []
    for line in text.splitlines(keepends=True):
        s = line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "")
        s = s.replace("\n", "\\n")
        out.append('    "' + s + '"')
    return "\n".join(out) if out else '    ""'


def main():
    src_dir, out_path = sys.argv[1], sys.argv[2]
    files = []
    for root, _, names in os.walk(src_dir):
        for n in sorted(names):
            if n.endswith(".lua"):
                full = os.path.join(root, n)
                files.append((os.path.relpath(full, src_dir).replace(os.sep, "/"), full))
    files.sort()
    lines = ["// Made by tools/embed_lua.py from psp/lua: edit those files, not this one", "",
             "static const struct {", "    const char *path;", "    const char *src;", "} k_lua_builtin[] = {"]
    for rel, full in files:
        with open(full, encoding="utf-8") as f:
            text = f.read()
        lines.append('{"%s",' % rel)
        lines.append(c_string(text) + "},")
    lines.append("};")
    data = "\n".join(lines) + "\n"
    old = None
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as f:
            old = f.read()
    if old != data: # unchanged: leave it be, so nothing rebuilds
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(data)
    print("%s: %d files" % (out_path, len(files)))


if __name__ == "__main__":
    main()
