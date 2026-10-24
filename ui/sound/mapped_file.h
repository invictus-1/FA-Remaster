// A file of the disk mapped into memory, read only: its bytes come from the disk as they are first
// touched, and they are the system's file cache, not memory of the program's own. The sound's wave
// banks are held this way, as the original holds them (sound_system.cpp, at LoadFolder).
// Where a file cannot be mapped (not a file of the disk of its own, or a system without mapping),
// FromBytes holds bytes read whole instead.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace moho::ui::sound {

class MappedFile {
 public:
  // The file at `hostPath`, mapped; nullptr when it cannot be (no such file, empty, no mapping).
  static std::shared_ptr<const MappedFile> Open(const std::string& hostPath);
  static std::shared_ptr<const MappedFile> FromBytes(std::string bytes);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool mapped() const { return mapped_; }  // (false: bytes read whole, in the program's own memory)

 private:
  MappedFile() = default;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  bool mapped_ = false;
  std::string bytes_;  // (FromBytes)
};

}  // namespace moho::ui::sound
