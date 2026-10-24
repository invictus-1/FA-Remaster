#include "sound/mapped_file.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace moho::ui::sound {

std::shared_ptr<const MappedFile> MappedFile::Open(const std::string& hostPath) {
  std::shared_ptr<MappedFile> file(new MappedFile());
#if defined(_WIN32)
  // (the path as the program's other file functions take it: fopen's, the system's code page)
  HANDLE handle = CreateFileA(hostPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER size{};
  HANDLE mapping = nullptr;
  if (GetFileSizeEx(handle, &size) && size.QuadPart > 0)
    mapping = CreateFileMappingA(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(handle);  // (the mapping keeps the file)
  if (!mapping) return nullptr;
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);  // (the view keeps the mapping)
  if (!view) return nullptr;
  file->size_ = static_cast<size_t>(size.QuadPart);
#else
  const int fd = ::open(hostPath.c_str(), O_RDONLY);
  if (fd < 0) return nullptr;
  struct stat st{};
  void* view = MAP_FAILED;
  if (::fstat(fd, &st) == 0 && st.st_size > 0) view = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // (the mapping keeps the file)
  if (view == MAP_FAILED) return nullptr;
  file->size_ = static_cast<size_t>(st.st_size);
#endif
  file->data_ = static_cast<const uint8_t*>(view);
  file->mapped_ = true;
  return file;
}

std::shared_ptr<const MappedFile> MappedFile::FromBytes(std::string bytes) {
  std::shared_ptr<MappedFile> file(new MappedFile());
  file->bytes_ = std::move(bytes);
  file->data_ = reinterpret_cast<const uint8_t*>(file->bytes_.data());
  file->size_ = file->bytes_.size();
  return file;
}

MappedFile::~MappedFile() {
  if (!mapped_ || !data_) return;
#if defined(_WIN32)
  UnmapViewOfFile(data_);
#else
  ::munmap(const_cast<uint8_t*>(data_), size_);
#endif
}

}  // namespace moho::ui::sound
