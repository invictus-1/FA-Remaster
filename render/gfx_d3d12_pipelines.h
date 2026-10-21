// [hitches] Direct3D 12's pipelines kept from one run to the next (gfx_d3d12.cpp; the driver's own compiled form, an
// ID3D12PipelineLibrary in a file beside the compiled shaders).
//
// Why: the shaders' own cache (DXC's DXIL, by their source) spares the compiling, but the driver still turns each
// pipeline's DXIL into the card's own code when it is made: measured on an RTX 4090 (2026-10-20), 0.6 to 1.8 s for each
// large pipeline (the models', the ground's, the water's, the grass's) the first time a new build of the program makes it
// (the driver's own cache is kept by program, and a new build is a new program), and 80 to 140 ms each time after. Most
// pipelines are made the first time their look is drawn, many of them while a game is under way (the first factory's
// pad, the first building's sparks), and each made then is a hitch of that much. Kept here, a pipeline the driver has
// made once is made again in a few milliseconds, by any build whose shaders came out the same.
//
// Each pipeline is kept under a name made from everything it is made of (its shaders' code, its state, the root
// signature), so a pipeline whose shaders or state changed is never taken for an older one. The file is the driver's:
// another driver or card refuses it (the library is then begun empty). Several programs may run at once: each reads the
// file as it starts and writes it back whole when it has added to it (written under another name first, then renamed,
// so a half-written file is never read); the last writer's is kept. A file grown past kMostBytes is begun afresh.
// Pictures are not changed by any of it: a pipeline taken from the library is the one the driver made.
#pragma once

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace render::gfx {

class D3D12PipelineStore {
 public:
  static constexpr uint64_t kMostBytes = 256ull << 20;
  ~D3D12PipelineStore() {
    if (library_) library_->Release();
  }
  // Opens the library kept in `folder` (empty: none is kept), for pipelines made with the root signature whose
  // serialized form is `rootSignature`.
  void Open(ID3D12Device* device, const std::string& folder, const void* rootSignature, size_t rootSignatureSize) {
    if (folder.empty() || !device) return;
    ID3D12Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12Device1), reinterpret_cast<void**>(&device1))) || !device1) return;
    path_ = folder + "\\pipelines-d3d12.bin";
    root_ = Hash(kSeed, rootSignature, rootSignatureSize);
    if (FILE* f = fopen(path_.c_str(), "rb")) {
      fseek(f, 0, SEEK_END);
      const long size = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (size > 0 && static_cast<uint64_t>(size) < kMostBytes) {
        blob_.resize(static_cast<size_t>(size));
        if (fread(blob_.data(), 1, blob_.size(), f) != blob_.size()) blob_.clear();
      }
      fclose(f);
    }
    HRESULT hr = E_FAIL;
    if (!blob_.empty()) hr = device1->CreatePipelineLibrary(blob_.data(), blob_.size(), __uuidof(ID3D12PipelineLibrary), reinterpret_cast<void**>(&library_));
    if (FAILED(hr)) {  // (none yet, or another driver's or card's: begun empty)
      if (!blob_.empty()) fprintf(stderr, "render: Direct3D 12: the kept pipelines are another driver's (0x%08lx): begun afresh\n", static_cast<unsigned long>(hr));
      blob_.clear();
      library_ = nullptr;
      hr = device1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), reinterpret_cast<void**>(&library_));
      if (FAILED(hr)) library_ = nullptr;  // (a driver without libraries: pipelines are made as before)
    }
    device1->Release();
  }

  // A graphics pipeline: taken from the library, else made (and added to it). The result as CreateGraphicsPipelineState's;
  // `kept`: it was taken from the library.
  HRESULT Graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, ID3D12PipelineState** out, bool* kept) {
    *kept = false;
    if (!library_) return device->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
    const std::wstring name = Name(KeyOf(d));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (SUCCEEDED(library_->LoadGraphicsPipeline(name.c_str(), &d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out)))) {
        *kept = true;
        return S_OK;
      }
    }
    const HRESULT hr = device->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
    if (SUCCEEDED(hr)) Store(name, *out);
    return hr;
  }
  HRESULT Compute(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& d, ID3D12PipelineState** out, bool* kept) {
    *kept = false;
    if (!library_) return device->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
    uint64_t h = Hash(root_ ^ 0xc0ull, d.CS.pShaderBytecode, d.CS.BytecodeLength);
    h = Mix(h, d.NodeMask);
    h = Mix(h, static_cast<uint64_t>(d.Flags));
    const std::wstring name = Name(h);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (SUCCEEDED(library_->LoadComputePipeline(name.c_str(), &d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out)))) {
        *kept = true;
        return S_OK;
      }
    }
    const HRESULT hr = device->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
    if (SUCCEEDED(hr)) Store(name, *out);
    return hr;
  }

  // Writes the library back to its file when pipelines were added since it was read or last written.
  void Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!library_ || !added_) return;
    added_ = 0;
    const SIZE_T size = library_->GetSerializedSize();
    if (!size || size >= kMostBytes) return;
    std::vector<uint8_t> data(size);
    if (FAILED(library_->Serialize(data.data(), size))) return;
    const std::string part = path_ + ".part" + std::to_string(GetCurrentProcessId());
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok || !MoveFileExA(part.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(part.c_str());
  }
  int Added() const { return added_; }

 private:
  static constexpr uint64_t kSeed = 1469598103934665603ull;
  static uint64_t Hash(uint64_t h, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
  }
  static uint64_t Mix(uint64_t h, uint64_t v) { return Hash(h, &v, sizeof v); }
  // Everything the pipeline is made of, field by field (a structure's padding is never read).
  uint64_t KeyOf(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    uint64_t h = Mix(root_, 0x9aull);
    for (const D3D12_SHADER_BYTECODE* s : {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS}) {
      h = Mix(h, s->BytecodeLength);
      h = Hash(h, s->pShaderBytecode, s->BytecodeLength);
    }
    h = Mix(h, d.StreamOutput.NumEntries);
    const D3D12_BLEND_DESC& b = d.BlendState;
    h = Mix(h, (static_cast<uint64_t>(b.AlphaToCoverageEnable) << 1) | static_cast<uint64_t>(b.IndependentBlendEnable));
    for (const D3D12_RENDER_TARGET_BLEND_DESC& t : b.RenderTarget)
      for (uint64_t v : {static_cast<uint64_t>(t.BlendEnable), static_cast<uint64_t>(t.LogicOpEnable), static_cast<uint64_t>(t.SrcBlend),
                         static_cast<uint64_t>(t.DestBlend), static_cast<uint64_t>(t.BlendOp), static_cast<uint64_t>(t.SrcBlendAlpha),
                         static_cast<uint64_t>(t.DestBlendAlpha), static_cast<uint64_t>(t.BlendOpAlpha), static_cast<uint64_t>(t.LogicOp),
                         static_cast<uint64_t>(t.RenderTargetWriteMask)})
        h = Mix(h, v);
    h = Mix(h, d.SampleMask);
    const D3D12_RASTERIZER_DESC& r = d.RasterizerState;
    for (uint64_t v : {static_cast<uint64_t>(r.FillMode), static_cast<uint64_t>(r.CullMode), static_cast<uint64_t>(r.FrontCounterClockwise),
                       static_cast<uint64_t>(static_cast<uint32_t>(r.DepthBias)), static_cast<uint64_t>(r.DepthClipEnable),
                       static_cast<uint64_t>(r.MultisampleEnable), static_cast<uint64_t>(r.AntialiasedLineEnable),
                       static_cast<uint64_t>(r.ForcedSampleCount), static_cast<uint64_t>(r.ConservativeRaster)})
      h = Mix(h, v);
    h = Hash(h, &r.DepthBiasClamp, sizeof r.DepthBiasClamp);
    h = Hash(h, &r.SlopeScaledDepthBias, sizeof r.SlopeScaledDepthBias);
    const D3D12_DEPTH_STENCIL_DESC& z = d.DepthStencilState;
    for (uint64_t v : {static_cast<uint64_t>(z.DepthEnable), static_cast<uint64_t>(z.DepthWriteMask), static_cast<uint64_t>(z.DepthFunc),
                       static_cast<uint64_t>(z.StencilEnable), static_cast<uint64_t>(z.StencilReadMask), static_cast<uint64_t>(z.StencilWriteMask)})
      h = Mix(h, v);
    for (const D3D12_DEPTH_STENCILOP_DESC* o : {&z.FrontFace, &z.BackFace})
      for (uint64_t v : {static_cast<uint64_t>(o->StencilFailOp), static_cast<uint64_t>(o->StencilDepthFailOp), static_cast<uint64_t>(o->StencilPassOp),
                         static_cast<uint64_t>(o->StencilFunc)})
        h = Mix(h, v);
    h = Mix(h, d.InputLayout.NumElements);
    for (UINT i = 0; i < d.InputLayout.NumElements; ++i) {
      const D3D12_INPUT_ELEMENT_DESC& e = d.InputLayout.pInputElementDescs[i];
      h = Hash(h, e.SemanticName, e.SemanticName ? std::strlen(e.SemanticName) + 1 : 0);
      for (uint64_t v : {static_cast<uint64_t>(e.SemanticIndex), static_cast<uint64_t>(e.Format), static_cast<uint64_t>(e.InputSlot),
                         static_cast<uint64_t>(e.AlignedByteOffset), static_cast<uint64_t>(e.InputSlotClass), static_cast<uint64_t>(e.InstanceDataStepRate)})
        h = Mix(h, v);
    }
    for (uint64_t v : {static_cast<uint64_t>(d.IBStripCutValue), static_cast<uint64_t>(d.PrimitiveTopologyType), static_cast<uint64_t>(d.NumRenderTargets),
                       static_cast<uint64_t>(d.DSVFormat), static_cast<uint64_t>(d.SampleDesc.Count), static_cast<uint64_t>(d.SampleDesc.Quality),
                       static_cast<uint64_t>(d.NodeMask), static_cast<uint64_t>(d.Flags)})
      h = Mix(h, v);
    for (DXGI_FORMAT f : d.RTVFormats) h = Mix(h, static_cast<uint64_t>(f));
    return h;
  }
  static std::wstring Name(uint64_t h) {
    wchar_t text[20];
    swprintf(text, 20, L"%016llx", static_cast<unsigned long long>(h));
    return text;
  }
  void Store(const std::wstring& name, ID3D12PipelineState* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (SUCCEEDED(library_->StorePipeline(name.c_str(), state))) ++added_;  // (a name there already: another's, left)
  }

  ID3D12PipelineLibrary* library_ = nullptr;
  std::vector<uint8_t> blob_;  // (what the library was made from: kept while it lives, as Direct3D asks)
  std::string path_;
  uint64_t root_ = 0;
  int added_ = 0;
  std::mutex mutex_;
};

}  // namespace render::gfx
