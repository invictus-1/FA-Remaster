// The size of a texture file, read from its header (no pixels are decoded here).
#pragma once
#include <string>

namespace moho {
class Vfs;
}

namespace moho::ui {

// Width and height in pixels of the image file at the virtual path (.dds, .png, .tga, .bmp, .jpg).
// False when the file is missing or not an image we can read; that is logged once per file as the
// original does ("Unable to load texture from file: ...").
bool TextureSize(const Vfs* vfs, const std::string& vpath, int* width, int* height);
// The sizes of the interface's textures taken ahead (every .dds under /textures/ui), for TextureSize to answer without
// reading their files: on a thread of its own from the program's start (app/main.cpp), as the lobby's making asks for
// some 1900 sizes at once (CMauiBitmap:SetNewTexture in CLobby:HostGame) and the game's screen, made in the frame its
// load ends in, for some 250, each a file read of 0.1 ms or more. Ours, only about time; a file that cannot be read is
// left for TextureSize to find (and log) as before.
void PrefetchTextureSizes(const Vfs* vfs);

}  // namespace moho::ui
