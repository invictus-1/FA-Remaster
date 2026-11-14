#!/usr/bin/env python3
"""check_log_print.py: every line the renderer prints goes through render/log_print.h [logsink].

A host (the interface's program) may install a sink that takes each of the renderer's lines into its own log
(render::SetLogSink, WorldPicture::SetLogSink). log_print.h turns printf( and fprintf( into the wrapper's calls,
and so a file that prints must include it, as the last of its includes (so that its macros never rewrite a system
or third-party header's declarations). A file that prints without it bypasses the sink: on a machine where the game
runs without a console, what it says is lost.

This lists, for each render/*.cpp and render/*.h (not render/third_party):
  - a call of printf( or fprintf( (std:: qualified or not) without #include "log_print.h";
  - an #include after "log_print.h" (a quoted .inc, a table of data included in the middle of the code, may follow);
  - std::printf( / std::fprintf( (the macros cannot rewrite those; write them unqualified);
  - other writes to the console: puts(, putchar(, vprintf(, std::cout, std::cerr, and fputs( / fputc( / putc( /
    fwrite( / vfprintf( / perror( with stdout or stderr among their arguments.
Comments and the contents of strings and characters (raw strings too: the shaders held as text) are left out.

Use: py -I render/tools/check_log_print.py [--list]   (from anywhere; --list also names every file that prints)
Prints nothing and exits 0 when every file is in order; else one line per finding and exit 1.
"""
import os
import re
import sys

RENDER = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Files that print but are not to include log_print.h, with the reason.
EXEMPT = {
    "log_print.h": "the wrapper itself: it defines the macros",
    "log_print.cpp": "the wrapper itself: it calls the real vprintf / vfprintf, and must not see the macros",
}


def strip(text):
    """The text with comments, string literals and character literals blanked (newlines kept, so that the
    line numbers stay); a preprocessor #include's <name> or "name" is kept."""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            out.append(c)
            i += 1
            line_start = True
            continue
        if line_start and c in " \t":
            out.append(c)
            i += 1
            continue
        if line_start and c == "#":
            m = re.match(r'#\s*include\s*(<[^>\n]*>|"[^"\n]*")', text[i:])
            if m:
                out.append(m.group(0))
                i += len(m.group(0))
                line_start = False
                continue
        line_start = False
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        m = re.match(r'(?:u8|[uUL])?R"([^ ()\\\t\n]{0,16})\(', text[i:])
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            end = ")" + m.group(1) + '"'
            j = text.find(end, i + len(m.group(0)))
            j = n if j < 0 else j + len(end)
            out.append('""' + re.sub(r"[^\n]", " ", text[i:j])[2:])
            i = j
            continue
        if c in "\"'":
            # (a digit separator 1'000 is not a character literal)
            if c == "'" and i > 0 and text[i - 1].isalnum() and re.match(r"[0-9a-fA-F]", text[i + 1:i + 2] or " ") \
                    and re.search(r"[0-9]\w*$", text[max(0, i - 32):i]):
                out.append(" ")
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + c if j - i >= 2 else c)
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


CALL = re.compile(r"(?<![\w.>:])(?:(std\s*::\s*)|::\s*)?\b(printf|fprintf)\s*\(")
CONSOLE = re.compile(r"(?<![\w.>])(?:std\s*::\s*)?\b(puts|putchar|vprintf)\s*\(|\bstd\s*::\s*(cout|cerr)\b")
STREAM_CALL = re.compile(r"(?<![\w.>])(?:std\s*::\s*)?\b(fputs|fputc|putc|fwrite|vfprintf|perror)\s*\(")
INCLUDE = re.compile(r'^[ \t]*#\s*include\s*([<"])([^>"]*)[>"]', re.M)


def args_of(code, start):
    """The text from `start` (just after a call's '(') to its closing ')'."""
    depth, j = 1, start
    while j < len(code) and depth:
        depth += {"(": 1, ")": -1}.get(code[j], 0)
        j += 1
    return code[start:j - 1]


def check(path, name, findings, printing):
    text = open(path, encoding="utf-8", errors="replace").read()
    code = strip(text)
    line = lambda pos: code.count("\n", 0, pos) + 1
    calls = list(CALL.finditer(code))
    includes = list(INCLUDE.finditer(code))
    ours = [m for m in includes if m.group(1) == '"' and m.group(2) == "log_print.h"]
    if calls:
        printing.append(name)
    if name in EXEMPT:
        return
    for m in calls:
        if m.group(1):
            findings.append(f"{name}:{line(m.start())}: std::{m.group(2)}( (write it unqualified, so the wrapper takes it)")
    if calls and not ours:
        findings.append(f"{name}:{line(calls[0].start())}: calls {calls[0].group(2)}( without #include \"log_print.h\"")
    if ours:
        # (a table of data included in the middle of the code, as fx_entities.cpp's sim/*.inc, is not a header)
        after = [m for m in includes if m.start() > ours[-1].start() and not (m.group(1) == '"' and m.group(2).endswith(".inc"))]
        if after:
            findings.append(f"{name}:{line(after[0].start())}: #include {after[0].group(0).split('include', 1)[1].strip()} "
                            "after \"log_print.h\" (it must be the last include)")
    for m in CONSOLE.finditer(code):
        findings.append(f"{name}:{line(m.start())}: {m.group(1) or 'std::' + m.group(2)} writes to the console past the wrapper")
    for m in STREAM_CALL.finditer(code):
        if m.group(1) == "perror" or re.search(r"\b(stdout|stderr)\b", args_of(code, m.end())):
            findings.append(f"{name}:{line(m.start())}: {m.group(1)}( to stdout / stderr past the wrapper")


def main():
    findings, printing = [], []
    for name in sorted(os.listdir(RENDER)):
        if name.endswith((".cpp", ".h")):
            check(os.path.join(RENDER, name), name, findings, printing)
    for f in findings:
        print(f)
    if "--list" in sys.argv[1:]:
        print(f"{len(printing)} files print:", " ".join(printing))
        print("exempt:", "; ".join(f"{k}: {v}" for k, v in EXEMPT.items()))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
