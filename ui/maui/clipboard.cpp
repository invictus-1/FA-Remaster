#include "maui/clipboard.h"
#include "maui/clipboard_native.h"

namespace moho::ui {

void CopyToClipboard(const std::string& utf8) { CopyToSystemClipboard(utf8); }
std::string ClipboardText() { return SystemClipboardText(); }

}  // namespace moho::ui
