# stb_truetype and stb_image

`stb_truetype.h` v1.26 by Sean Barrett, from github.com/nothings/stb at commit
6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760 (sha256 ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab),
unchanged. Public domain or MIT, as the end of the file says. It draws the game's own .ttf fonts
on Linux and Mac (`ui/maui/font.cpp`); Windows draws them with GDI.

`stb_image.h` v2.30 by Sean Barrett, from github.com/nothings/stb at commit
013ac3beddff3dbffafd5177e7972067cd2b5083 (the last change to that file, 2024-05-31; sha256
594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3), unchanged. Public domain or MIT, as the end of the
file says. Only its JPEG reader is compiled (`ui/draw/textures.cpp`: mods' .jpg icons in the mods manager); added
with Chris's OK of 2026-10-19.
