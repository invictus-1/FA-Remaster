"""Writes render/fsr3_source.cpp: the compute passes of AMD's FSR 3 upscaler (the FidelityFX SDK v2.3.0's
fsr3upscaler, MIT, see LICENSE.txt and README.md here) as C++ strings, for the shaders the renderer compiles at
run time (fsr3.cpp), which has no include handler. Run it again when the files here change:

    py -I render/third_party/fsr3/embed_fsr3.py

Each pass's #includes are followed recursively, as the preprocessor would (by file name: the files lie flat in
this folder), and a file with an include guard is taken only once a pass. The text of each file is cut at its
#include lines into parts that are written once and shared by every pass that uses them; a pass is the list of
its parts in order, which fsr3.cpp joins. Each part begins with a #line naming its file, so that the compiler's
messages point into the files here. The three headers of AMD's core that these passes never use (the processor's,
GLSL's and 16-bit floats') are not copied: their #include becomes an #error under the condition that would need
them."""
import io, os, re

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "..", "..", "fsr3_source.cpp")

# (pass file, the name of its list in C++)
PASSES = [
    ("ffx_fsr3upscaler_prepare_inputs_pass.hlsl", "kFsr3PrepareInputs"),
    ("ffx_fsr3upscaler_luma_pyramid_pass.hlsl", "kFsr3LumaPyramid"),
    ("ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl", "kFsr3ShadingChangePyramid"),
    ("ffx_fsr3upscaler_shading_change_pass.hlsl", "kFsr3ShadingChange"),
    ("ffx_fsr3upscaler_prepare_reactivity_pass.hlsl", "kFsr3PrepareReactivity"),
    ("ffx_fsr3upscaler_luma_instability_pass.hlsl", "kFsr3LumaInstability"),
    ("ffx_fsr3upscaler_accumulate_pass.hlsl", "kFsr3Accumulate"),
]
# headers left out, and the condition under which a pass would need them
LEFT_OUT = {
    "ffx_core_cpu.h": "defined(FFX_CPU)",
    "ffx_core_glsl.h": "defined(FFX_GLSL)",
    "ffx_core_gpu_common_half.h": "FFX_HALF",
}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"\s*$')


def read(name):
    return io.open(os.path.join(here, name), encoding="ascii", newline="").read().replace("\r\n", "\n")


def guarded(text):
    """True when the whole file is under an include guard (#pragma once, or #ifndef X / #if !defined(X) with
    #define X right after, closed by the file's last #endif)."""
    lines = [l.strip() for l in text.split("\n")]
    code = [l for l in lines if l and not l.startswith("//")]
    if any(l == "#pragma once" for l in code):
        return True
    if len(code) < 3 or not code[-1].startswith("#endif"):
        return False
    m = re.match(r"#\s*(?:ifndef\s+(\w+)|if\s+!\s*defined\s*\(\s*(\w+)\s*\))\s*$", code[0])
    return bool(m) and code[1].split()[:2] == ["#define", m.group(1) or m.group(2)]


parts = {}       # (file, index) -> text
part_order = []  # in the order first met


def add_part(key, text):
    if key not in parts:
        parts[key] = text
        part_order.append(key)


def walk(name, taken, sequence):
    """Appends to `sequence` the parts of `name` with what it includes inlined."""
    if name in LEFT_OUT:
        key = (name, "left out")
        add_part(key, "#if %s\n#error \"%s is not embedded (render/third_party/fsr3/embed_fsr3.py)\"\n#endif\n" % (LEFT_OUT[name], name))
        sequence.append(key)
        return
    if not os.path.exists(os.path.join(here, name)):
        raise SystemExit("embed_fsr3: %s is included but not here" % name)
    text = read(name)
    if guarded(text):
        if name in taken:
            return
        taken.add(name)
    lines = text.split("\n")
    start, index = 0, 0
    for i, line in enumerate(lines + [None]):
        m = INCLUDE.match(line) if line is not None else None
        if line is not None and not m:
            continue
        key = (name, index)
        add_part(key, '#line %d "%s"\n' % (start + 1, name) + "\n".join(lines[start:i]) + "\n")
        sequence.append(key)
        index += 1
        if m:
            walk(os.path.basename(m.group(1)), taken, sequence)
            start = i + 1


def literal(text):
    # adjacent raw strings of at most 12000 characters each (one string in the end)
    if ')ffx"' in text:
        raise SystemExit("embed_fsr3: the text holds the raw string's end")
    pieces = [text[i:i + 12000] for i in range(0, len(text), 12000)] or [""]
    return "\n".join('R"ffx(' + p + ')ffx"' for p in pieces)


lists = []
for file, symbol in PASSES:
    sequence = []
    walk(file, set(), sequence)
    lists.append((symbol, file, sequence))

names = {key: "kPart%d" % i for i, key in enumerate(part_order)}
body = []
for key in part_order:
    what = "%s, %s" % (key[0], key[1] if key[1] == "left out" else "part %d" % key[1])
    body.append("// %s\nconst char %s[] =\n%s;\n" % (what, names[key], literal(parts[key])))
for symbol, file, sequence in lists:
    body.append("// %s\nextern const char* const %s[] = {%s, nullptr};\n" % (file, symbol, ", ".join(names[k] for k in sequence)))

io.open(out, "w", encoding="ascii", newline="\n").write(
    "// Generated by render/third_party/fsr3/embed_fsr3.py from AMD's FidelityFX SDK v2.3.0 (the FSR 3.1 upscaler's\n"
    "// compute passes; Copyright (C) 2026 Advanced Micro Devices, Inc.; MIT license: render/third_party/fsr3/LICENSE.txt).\n"
    "// Do not edit; run the script again.\n"
    "namespace render {\n"
    "namespace {\n\n" + "\n".join(body[:len(part_order)]) + "\n}  // namespace\n\n"
    "// Each pass's source as its parts in order (null at the end); fsr3.cpp joins them.\n" + "\n".join(body[len(part_order):]) +
    "\n}  // namespace render\n")
print("wrote", os.path.normpath(out), "-", len(part_order), "parts,", sum(len(t) for t in parts.values()), "characters")
