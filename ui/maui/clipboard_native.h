#pragma once
#include <string>

namespace moho::ui {
void CopyToSystemClipboard(const std::string& utf8);
std::string SystemClipboardText();
}
