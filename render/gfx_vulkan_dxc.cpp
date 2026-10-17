#include "gfx_vulkan_dxc.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>

namespace render::gfx {
namespace {

// --- the part of DXC's interface used (dxcapi.h: IDxcCompiler3, IDxcResult, IDxcBlob) ---
struct Guid {
  uint32_t a;
  uint16_t b, c;
  uint8_t d[8];
};
constexpr Guid kClsidCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr Guid kIidCompiler3 = {0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
constexpr Guid kIidResult = {0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};

struct DxcUnknown {
  virtual int32_t QueryInterface(const Guid& iid, void** out) = 0;
  virtual uint32_t AddRef() = 0;
  virtual uint32_t Release() = 0;
#if !defined(_WIN32)
  // (outside Windows DXC's IUnknown has a virtual destructor: two entries of the table)
  virtual void DestroyComplete() = 0;
  virtual void DestroyDeleting() = 0;
#endif
};
struct DxcBlob : DxcUnknown {
  virtual void* GetBufferPointer() = 0;
  virtual size_t GetBufferSize() = 0;
};
struct DxcBlobEncoding : DxcBlob {
  virtual int32_t GetEncoding(int32_t* known, uint32_t* codePage) = 0;
};
struct DxcOperationResult : DxcUnknown {
  virtual int32_t GetStatus(int32_t* status) = 0;
  virtual int32_t GetResult(DxcBlob** result) = 0;
  virtual int32_t GetErrorBuffer(DxcBlobEncoding** errors) = 0;
};
struct DxcSource {
  const void* ptr;
  size_t size;
  uint32_t encoding;  // 65001: UTF-8
};
struct DxcCompiler3 : DxcUnknown {
  virtual int32_t Compile(const DxcSource* source, const wchar_t** arguments, uint32_t argCount, void* includeHandler, const Guid& iid,
                          void** result) = 0;
  virtual int32_t Disassemble(const DxcSource* object, const Guid& iid, void** result) = 0;
};
using CreateInstanceFn = int32_t (*)(const Guid& clsid, const Guid& iid, void** out);

template <typename T>
struct Ref {
  T* p = nullptr;
  ~Ref() {
    if (p) p->Release();
  }
};

DxcCompiler3* Compiler(std::string* error) {
  static DxcCompiler3* compiler = nullptr;
  static bool tried = false;
  if (tried) {
    if (!compiler && error) *error = "DXC (the shader compiler) could not be loaded";
    return compiler;
  }
  tried = true;
  CreateInstanceFn create = nullptr;
#if defined(_WIN32)
  if (HMODULE lib = LoadLibraryA("dxcompiler.dll")) create = reinterpret_cast<CreateInstanceFn>(reinterpret_cast<void*>(GetProcAddress(lib, "DxcCreateInstance")));
#elif defined(__APPLE__)
  if (void* lib = dlopen("libdxcompiler.dylib", RTLD_NOW | RTLD_LOCAL)) create = reinterpret_cast<CreateInstanceFn>(dlsym(lib, "DxcCreateInstance"));
#else
  if (void* lib = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL)) create = reinterpret_cast<CreateInstanceFn>(dlsym(lib, "DxcCreateInstance"));
#endif
  void* made = nullptr;
  if (create && create(kClsidCompiler, kIidCompiler3, &made) >= 0) compiler = static_cast<DxcCompiler3*>(made);
  if (!compiler && error) *error = "DXC (the shader compiler, dxcompiler) not found beside the program";
  return compiler;
}

// The renderer's registers on Vulkan's descriptor sets (gfx_vulkan.cpp lays them out the same way):
// set 0: t0-t39 at 16-55, t0-t1 of space1 at 56-57, s0-s5 at 64-69, a compute shader's u0-u15 at 80-95;
// set 1: b0-b3 at 0-3.
// The shader model: 6.0, every card's driver of the last years takes it; 6.5 for a shader that traces rays
// (RayQuery).
const wchar_t* ShaderProfile(const char* stage, bool rays) {
  if (std::strcmp(stage, "vs") == 0) return rays ? L"vs_6_5" : L"vs_6_0";
  if (std::strcmp(stage, "cs") == 0) return rays ? L"cs_6_5" : L"cs_6_0";
  return rays ? L"ps_6_5" : L"ps_6_0";
}

std::vector<std::wstring> Arguments(const char* entry, const char* stage, bool rays) {
  std::vector<std::wstring> a = {L"-spirv", L"-fspv-target-env=vulkan1.3", L"-fvk-use-dx-layout", L"-O3", L"-fspv-reflect",
                                 L"-HV", L"2018",  // (the shaders are written for Direct3D 12's compiler: HLSL before 2021's rules)
                                 L"-E"};
  a.push_back(std::wstring(entry, entry + std::strlen(entry)));
  a.push_back(L"-T");
  a.push_back(ShaderProfile(stage, rays));
  if (std::strcmp(stage, "vs") == 0) a.push_back(L"-fvk-invert-y");  // (Vulkan's y runs down the screen)
  // (a pixel's SV_Position.w is the distance's w in Direct3D, 1 / w in Vulkan: particles' soft edges read it)
  if (std::strcmp(stage, "ps") == 0) a.push_back(L"-fvk-use-dx-position-w");
  auto bind = [&](const std::wstring& reg, int space, int binding, int set) {
    a.push_back(L"-fvk-bind-register");
    a.push_back(reg);
    a.push_back(std::to_wstring(space));
    a.push_back(std::to_wstring(binding));
    a.push_back(std::to_wstring(set));
  };
  for (int i = 0; i < 4; ++i) bind(L"b" + std::to_wstring(i), 0, i, 1);
  for (int i = 0; i < 40; ++i) bind(L"t" + std::to_wstring(i), 0, 16 + i, 0);
  for (int i = 0; i < 2; ++i) bind(L"t" + std::to_wstring(i), 1, 56 + i, 0);
  for (int i = 0; i < 6; ++i) bind(L"s" + std::to_wstring(i), 0, 64 + i, 0);
  if (std::strcmp(stage, "cs") == 0)  // (only compute shaders write; the others' arguments stay as they were)
    for (int i = 0; i < 16; ++i) bind(L"u" + std::to_wstring(i), 0, 80 + i, 0);
  return a;
}

// SPIR-V: the vertex inputs' semantics and locations read (DXC's -fspv-reflect decorations), then those
// decorations and their extensions taken out again (not every driver takes them); and the textures of
// set 0 with how many each binding holds.
bool Reflect(const std::vector<uint32_t>& in, SpirvShader* out) {
  if (in.size() < 5 || in[0] != 0x07230203u) return false;
  std::vector<std::pair<uint32_t, uint32_t>> locations;      // id, location
  std::vector<std::pair<uint32_t, std::string>> semantics;   // id, semantic
  std::vector<uint32_t> inputIds;
  std::map<uint32_t, uint32_t> bindings, sets, pointee, arrays, constants;  // id -> binding, set, type, length id, value
  std::map<uint32_t, uint32_t> images;  // image type id -> SpirvImage::Kind
  std::vector<uint32_t> resources;      // variables of resources
  out->words.assign(in.begin(), in.begin() + 5);
  for (size_t at = 5; at < in.size();) {
    const uint32_t count = in[at] >> 16, op = in[at] & 0xffff;
    if (count == 0 || at + count > in.size()) return false;
    auto text = [&](size_t from) {
      std::string s;
      for (size_t w = from; w < at + count; ++w)
        for (int b = 0; b < 4; ++b) {
          const char c = static_cast<char>((in[w] >> (8 * b)) & 0xff);
          if (!c) return s;
          s.push_back(c);
        }
      return s;
    };
    bool keep = true;
    if (op == 71 && count >= 4 && in[at + 2] == 30) locations.push_back({in[at + 1], in[at + 3]});  // OpDecorate Location
    if (op == 59 && count >= 4 && in[at + 3] == 1) inputIds.push_back(in[at + 2]);                  // OpVariable Input
    if (op == 71 && count >= 4 && in[at + 2] == 33) bindings[in[at + 1]] = in[at + 3];              // OpDecorate Binding
    if (op == 71 && count >= 4 && in[at + 2] == 34) sets[in[at + 1]] = in[at + 3];                  // OpDecorate DescriptorSet
    if (op == 59 && count >= 4 && in[at + 3] == 0) {                                                // OpVariable UniformConstant
      resources.push_back(in[at + 2]);
      pointee[in[at + 2]] = in[at + 1];
    }
    if (op == 32 && count >= 4) pointee[in[at + 1]] = in[at + 3];                                   // OpTypePointer
    if (op == 28 && count >= 4) arrays[in[at + 1]] = in[at + 3];                                    // OpTypeArray
    if (op == 43 && count >= 4) constants[in[at + 2]] = in[at + 3];                                 // OpConstant
    if (op == 25 && count >= 9) {                                                                   // OpTypeImage: dim, depth, arrayed, multi-sampled, sampled
      const uint32_t dim = in[at + 3];  // (1 two dimensions, 2 three, 3 a cube)
      images[in[at + 1]] = in[at + 7] == 2 ? (dim == 2 ? SpirvImage::StorageVolume : SpirvImage::Storage)  // (sampled 2: written, a storage image)
                           : dim == 3      ? SpirvImage::Cube
                           : dim == 2      ? SpirvImage::Volume
                           : in[at + 6]    ? SpirvImage::MultiSample
                           : in[at + 5]    ? SpirvImage::Array
                                           : SpirvImage::Plain;
    }
    if (op == 5632 || op == 5633) {  // OpDecorateString, OpMemberDecorateString (UserSemantic, UserTypeGOOGLE)
      if (op == 5632 && count >= 4 && in[at + 2] == 5635) {
        std::string s = text(at + 3);
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        semantics.push_back({in[at + 1], s});
      }
      keep = false;
    }
    if (op == 10) {  // OpExtension
      const std::string name = text(at + 1);
      if (name == "SPV_GOOGLE_hlsl_functionality1" || name == "SPV_GOOGLE_user_type") keep = false;
    }
    if (keep) out->words.insert(out->words.end(), in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + count));
    at += count;
  }
  for (const auto& s : semantics) {
    bool input = false;
    for (uint32_t id : inputIds) input = input || id == s.first;
    if (!input) continue;
    for (const auto& l : locations)
      if (l.first == s.first) {
        std::string semantic = s.second;
        if (semantic.empty() || !std::isdigit(static_cast<unsigned char>(semantic.back()))) semantic += "0";
        out->inputs.push_back({semantic, l.second});
      }
  }
  for (uint32_t v : resources) {
    if (!bindings.count(v) || (sets.count(v) && sets[v] != 0)) continue;
    uint32_t type = pointee.count(v) && pointee.count(pointee[v]) ? pointee[pointee[v]] : 0;  // (variable -> pointer -> type)
    uint32_t n = 1;
    if (arrays.count(type)) {
      n = constants.count(arrays[type]) ? constants[arrays[type]] : 1;
      // (the array's element type: the instruction's second word)
      for (size_t at = 5; at < in.size(); at += in[at] >> 16)
        if ((in[at] & 0xffff) == 28 && in[at + 1] == type) {
          type = in[at + 2];
          break;
        }
    }
    if (images.count(type)) out->images.push_back({bindings[v], n, images[type]});
  }
  return true;
}

// The file a compile is kept in, under a hash of everything that goes into it (empty: none kept).
std::string CacheFile(const std::string& cacheDir, const char* version, const std::string& source, const std::vector<std::wstring>& args,
                      const char* extension) {
  if (cacheDir.empty()) return {};
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const void* data, size_t size) {
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
  };
  mix(version, std::strlen(version) + 1);
  mix(source.data(), source.size());
  for (const std::wstring& a : args) mix(a.data(), a.size() * sizeof(wchar_t) + sizeof(wchar_t));
  char name[48];
  snprintf(name, sizeof name, "%016llx.%s", static_cast<unsigned long long>(h), extension);
  return cacheDir + "/" + name;
}

bool ReadWhole(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize(size > 0 ? static_cast<size_t>(size) : 0);
  const bool read = !out->empty() && fread(out->data(), 1, out->size(), f) == out->size();
  fclose(f);
  return read;
}

// (under another name first: a half-written file is never read)
void WriteWhole(const std::string& path, const void* data, size_t size) {
  const std::string part = path + ".part";
  if (FILE* f = fopen(part.c_str(), "wb")) {
    const bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    std::remove(path.c_str());
    if (!ok || std::rename(part.c_str(), path.c_str()) != 0) std::remove(part.c_str());
  }
}

// DXC run on `source` with `args`: what it made in `out`, or false with its messages in `error`
// (`missing`: DXC itself is not there).
bool Run(const std::string& source, const std::vector<std::wstring>& args, std::vector<uint8_t>* out, std::string* error, bool* missing) {
  DxcCompiler3* compiler = Compiler(error);
  if (!compiler) {
    if (missing) *missing = true;
    return false;
  }
  std::vector<const wchar_t*> argv;
  for (const std::wstring& a : args) argv.push_back(a.c_str());
  DxcSource src{source.data(), source.size(), 65001};
  Ref<DxcOperationResult> result;
  void* made = nullptr;
  if (compiler->Compile(&src, argv.data(), static_cast<uint32_t>(argv.size()), nullptr, kIidResult, &made) < 0 || !made) {
    if (error) *error = "DXC did not run";
    return false;
  }
  result.p = static_cast<DxcOperationResult*>(made);
  int32_t status = -1;
  result.p->GetStatus(&status);
  if (status < 0) {
    Ref<DxcBlobEncoding> errors;
    result.p->GetErrorBuffer(&errors.p);
    if (error)
      *error = errors.p && errors.p->GetBufferSize() ? std::string(static_cast<const char*>(errors.p->GetBufferPointer()), errors.p->GetBufferSize())
                                                     : std::string("DXC failed");
    return false;
  }
  Ref<DxcBlob> blob;
  result.p->GetResult(&blob.p);
  if (!blob.p || blob.p->GetBufferSize() < 20) {
    if (error) *error = "DXC gave nothing";
    return false;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(blob.p->GetBufferPointer());
  out->assign(bytes, bytes + blob.p->GetBufferSize());
  return true;
}

}  // namespace

bool CompileSpirv(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, SpirvShader* out,
                  std::string* error) {
  const std::vector<std::wstring> args = Arguments(entry, stage, source.find("RayQuery") != std::string::npos);
  // kept from an earlier run when nothing that goes into it has changed
  const std::string cached = CacheFile(cacheDir, "dxc-spirv/2", source, args, "spv");
  std::vector<uint8_t> bytes;
  auto words = [&bytes] {
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    return w;
  };
  if (!cached.empty() && ReadWhole(cached, &bytes) && Reflect(words(), out)) return true;
  *out = SpirvShader{};
  if (!Run(source, args, &bytes, error, nullptr)) return false;
  if (!Reflect(words(), out)) {
    if (error) *error = "Vulkan: DXC's SPIR-V could not be read";
    return false;
  }
  if (!cached.empty()) WriteWhole(cached, bytes.data(), bytes.size() / 4 * 4);
  return true;
}

bool CompileDxil(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, std::vector<uint8_t>* out,
                 std::string* error, bool* missing) {
  std::vector<std::wstring> args = {L"-O3", L"-HV", L"2018", L"-E"};  // (HLSL 2018's rules, as for Vulkan)
  args.push_back(std::wstring(entry, entry + std::strlen(entry)));
  args.push_back(L"-T");
  args.push_back(ShaderProfile(stage, source.find("RayQuery") != std::string::npos));
  // (signed: the container's digest, the 16 bytes after "DXBC", is all nought where nothing signed it, and
  // Direct3D 12 takes no shader that is not; DXC of 2026 signs by itself, older ones through dxil.dll)
  auto isSigned = [](const std::vector<uint8_t>& code) {
    if (code.size() < 20 || std::memcmp(code.data(), "DXBC", 4) != 0) return false;
    for (size_t i = 4; i < 20; ++i)
      if (code[i]) return true;
    return false;
  };
  const std::string cached = CacheFile(cacheDir, "dxc-dxil/1", source, args, "dxil");
  if (!cached.empty() && ReadWhole(cached, out) && isSigned(*out)) return true;
  if (!Run(source, args, out, error, missing)) return false;
  if (!isSigned(*out)) {
    if (error) *error = "DXC's shaders come out unsigned (an older DXC needs its dxil.dll beside the program)";
    if (missing) *missing = true;
    return false;
  }
  if (!cached.empty()) WriteWhole(cached, out->data(), out->size());
  return true;
}

}  // namespace render::gfx
