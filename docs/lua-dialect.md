# The game's Lua dialect

Supreme Commander: Forged Alliance runs its scripts on a modified Lua 5.0.1 (GPG's build of
LuaPlus). Mods and FAF's own code rely on its differences from stock Lua, so this engine
reproduces them in `third_party/lua`. Every item below was checked against the original
binaries and is covered by `tests/dialect_test.lua`.

## Numbers
- `lua_Number` is a 32-bit `float`. `tostring(0.1)` is `"0.10000000149012"` (`%.14g` of the float).
- Number formatting follows the MSVC 2005 C runtime: exponents have three digits
  (`1.0000000200409e+020`), and `string.format("%02s", "9")` pads with zeros (`"09"`).
- `tonumber` accepts decimal notation only (no `0x10`, `inf` or `nan` strings).

## Syntax
- `!=` is a synonym for `~=`.
- `#` starts a line comment. There is no length operator, so Lua 5.1 code using `#t` fails to compile, as it does in the original.
- `continue` skips to the next iteration of `while`, `repeat` (to the `until` test) and `for` loops.
- `break n` / `continue n` act on the n-th enclosing loop.
- Hexadecimal integer literals: `0x1F` (at most 8 digits).
- Table constructor size hints: `{&hashsize &arraysize ...}`, e.g. `{&1&4}`.
- A UTF-8 byte order mark is skipped.
- `for k, v in t do` (iterating a table without `pairs`) works, as in Lua 5.0.

## Operators
- Bitwise `&`, `|`, `<<`, `>>`. All four bind tighter than `*` and `/`: their priority is 8, against 7 for `*` and `/`.
- `^` is bitwise **xor**, not power. There is no global `__pow`; use `math.pow`.
- Bitwise operands must be numbers; strings are not converted. Each operand is rounded to the nearest integer, with ties going to the even neighbour: `2.5 ^ 0 == 2` and `3.5 ^ 0 == 4`. The operation runs on 32 bits, and the result reads back as an unsigned integer: `-1 & -1 == 4294967295`.
- Comparing values of different types does not raise an error. The values are ordered by their type tag: nil < boolean < light userdata < number < string < table < function < userdata < thread. So `nil < 0` is `true`.

## Default metatables
- Every non-table, non-userdata type has a default metatable: `getmetatable(nil)`,
  `getmetatable(0)`, `getmetatable('')`, `getmetatable(coroutine.create(f))` are tables.
- The string library table *is* the strings' default metatable, so `s:find(...)` works and
  `getmetatable('') == string`.
- For these types, the VM uses the default metatable itself as the tag method for every event.
  - **Indexing:** reading `v.key` on nil, a number, a boolean and so on looks `key` up in the
    type's default metatable, then follows that table's `__index`. A missing key yields nil, with
    no error. In particular, indexing nil returns nil, and much mod code depends on that.
  - **Assignment:** `v.key = x` stores into the default metatable. FAF's `config.lua` blocks this
    with a `__newindex` meta-metatable.
  - **Arithmetic:** a number on the left hides the right operand's metamethods, so `5 + t` fails
    even when `t` has `__add`.

## Library differences
- `type(f)` returns `"cfunction"` for C functions.
- `io.dir(pattern)` lists names that match a Win32 wildcard, including `.` and `..`.

## Engine behaviour around scripts (not the VM)
- `doscript(name)` loads the file, then appends every hook file into **one chunk**, so hooks can
  see the file's locals. The hooks come from the init file's `hook` dirs (`/schook`), then from each
  active mod's `<location>/hook`. Each hook is logged as `Hooked <name> with <hook>`.
- The chunk name is the host path in the original's form, for example
  `@c:\users\...\gamedata\lua.nx2\lua\siminit.lua`. Some mods print or parse this name.
- Virtual paths are rooted. A relative path such as `mods/x/y.sca` is not found by `exists` or
  `DiskGetFileInfo`.
