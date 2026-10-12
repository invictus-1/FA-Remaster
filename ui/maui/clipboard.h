// The system clipboard as text, for the edit field's copy, cut and paste (the original's
// WIN_CopyToClipboard 0x4f2730 and WIN_GetClipboardText 0x4f25d0). UTF-8 on our side. On hosts
// without a clipboard the text is kept inside the process.
#pragma once
#include <string>

namespace moho::ui {

void CopyToClipboard(const std::string& utf8);
std::string ClipboardText();

}  // namespace moho::ui
