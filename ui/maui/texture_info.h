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

}  // namespace moho::ui
