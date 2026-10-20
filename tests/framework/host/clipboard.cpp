#include "maui/clipboard.h"
#include "maui/clipboard_native.h"
#include "tests/framework/host/clipboard.h"

namespace moho::ui {
namespace {
bool memoryClipboard = true;
std::string contents;
bool saved = false;
std::string original;
}
namespace testing {
void UseMemoryClipboard(bool enabled) { memoryClipboard = enabled; }
void RestoreClipboard() {
  if (saved) CopyToSystemClipboard(original);
  saved = false;
  original.clear();
}
}

void CopyToClipboard(const std::string& utf8) {
  if (memoryClipboard) contents = utf8;
  else {
    if (!saved) original = SystemClipboardText(), saved = true;
    CopyToSystemClipboard(utf8);
  }
}

std::string ClipboardText() {
  return memoryClipboard ? contents : SystemClipboardText();
}
}
